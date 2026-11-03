#include "lorawan.h"
#include "version11.h"
#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace obrii::lorawan {
namespace {
void available(bool busy, bool failed) {
    if(busy) throw Error("Reentrant session mutation during persistence");
    if(failed) throw Error("Persistence failed; reload durable state before continuing");
}
template<class State, class Persist>
void commit(State &state, const State &next, Persist &persist, bool &busy, bool &failed) {
    static_assert(std::is_nothrow_move_assignable<State>::value,"State commit must not throw after persistence");
    available(busy,failed);
    State prepared=next;
    busy=true;
    try { persist(prepared); }
    catch(...) { failed=true; busy=false; throw; }
    state=std::move(prepared);
    busy=false;
}
SessionState activated(const JoinAccept &a, const Credentials &credentials, std::uint16_t nonce, Role role) {
    SessionState session; session.role=role; session.devAddr=a.devAddr;
    session.keys=credentials.version==Version::V1_1 ?
        deriveSessionKeys11(credentials.nwkKey,credentials.appKey,a.joinNonce,credentials.joinEui,nonce):
        deriveSessionKeys(credentials.appKey,a.joinNonce,a.netId,nonce); return session;
}
Direction outgoing(Role role) { return role==Role::Device ? Direction::Uplink:Direction::Downlink; }
bool applicationDown(const DataFrame &f,const SessionKeys &keys) {
    return keys.version==Version::V1_1 && direction(f.type)==Direction::Downlink && f.port && *f.port>0;
}
bool applicationDown(const Bytes &wire,const SessionKeys &keys) {
    (void)wireCounter(wire);
    const auto at=std::size_t(8+(wire[5]&15));
    return keys.version==Version::V1_1 && direction(FrameType(wire[0]>>5))==Direction::Downlink && at<wire.size()-4 && wire[at]>0;
}
DataContext contextFor(const SessionState &s,DataContext context,bool transmit) {
    const auto count=transmit?s.confirmedReceived:s.confirmedTransmitted;
    context.confirmedCounter=count?std::uint16_t(*count):0;return context;
}
void checkUpdate(const std::optional<SessionState> &previous, const SessionState &next) {
    (void)saveSession(next);
    if(!previous || previous->role!=next.role || previous->devAddr!=next.devAddr
       || previous->keys.network!=next.keys.network || previous->keys.application!=next.keys.application
       || previous->keys.version!=next.keys.version || previous->keys.servingIntegrity!=next.keys.servingIntegrity
       || previous->keys.networkEncryption!=next.keys.networkEncryption
       || next.nextTransmit<previous->nextTransmit
       || next.nextApplicationTransmit<previous->nextApplicationTransmit
       || (previous->lastApplicationReceived && (!next.lastApplicationReceived || *next.lastApplicationReceived<*previous->lastApplicationReceived))
       || (previous->lastReceived && (!next.lastReceived || *next.lastReceived<*previous->lastReceived)))
        throw Error("Session update would change key context or roll back counters");
    if(next.nextTransmit==previous->nextTransmit && next.nextApplicationTransmit==previous->nextApplicationTransmit
       && ((next.lastTransmitted!=previous->lastTransmitted && !(next.keys.version==Version::V1_1 &&
               next.transmitCopies>previous->transmitCopies && next.lastTransmitted.size()==previous->lastTransmitted.size() &&
               next.lastTransmitted.size()>=12 && std::equal(next.lastTransmitted.begin(),next.lastTransmitted.end()-4,previous->lastTransmitted.begin()))) || next.transmitCopies<previous->transmitCopies
           || (previous->acknowledgmentPending && !next.acknowledgmentPending)))
        throw Error("Stale session transmit state");
    if(next.lastReceived==previous->lastReceived
       && (next.lastUplink!=previous->lastUplink || next.uplinkCopies<previous->uplinkCopies))
        throw Error("Stale session receive state");
}
}
Session::Session(Role role, SessionState restored, Persist persist)
    : role_(role),state_(std::move(restored)),persist_(std::move(persist)) {
    if(!persist_ || role_!=state_.role || (role_!=Role::Device && role_!=Role::Network))
        throw Error("Session requires matching role and a durable persistence callback");
    (void)saveSession(state_); // Validate restored counters and cache bounds.
}
void Session::commit(const SessionState &next) { ::obrii::lorawan::commit(state_,next,persist_,busy_,failed_); }
Bytes Session::preview(DataFrame frame,DataContext context) const {
    available(busy_,failed_);
    if(direction(frame.type)!=outgoing(role_))throw Error("Wrong outgoing frame direction");
    const auto count=applicationDown(frame,state_.keys)?state_.nextApplicationTransmit:state_.nextTransmit;
    if(count>0xffffffffULL)throw Error("Frame counter exhausted; activate with new session keys");
    frame.devAddr=state_.devAddr;frame.counter=std::uint32_t(count);
    if(state_.acknowledgmentPending)frame.control|=0x20;
    else if(state_.keys.version==Version::V1_1)frame.control&=0xdf;
    return encodeData(frame,state_.keys,contextFor(state_,context,true));
}
Bytes Session::transmit(DataFrame frame,DataContext context) {
    available(busy_,failed_);
    if(direction(frame.type)!=outgoing(role_)) throw Error("Wrong outgoing frame direction");
    const auto count=applicationDown(frame,state_.keys)?state_.nextApplicationTransmit:state_.nextTransmit;
    if(count>0xffffffffULL) throw Error("Frame counter exhausted; activate with new session keys");
    frame.devAddr=state_.devAddr;
    frame.counter=std::uint32_t(count);
    if(state_.acknowledgmentPending) frame.control|=0x20;
    else if(state_.keys.version==Version::V1_1)frame.control&=0xdf;
    context=contextFor(state_,context,true);
    const auto wire=encodeData(frame,state_.keys,context);
    auto next=state_; if(applicationDown(frame,state_.keys))++next.nextApplicationTransmit;else ++next.nextTransmit;
    next.acknowledgmentPending=false;
    if(confirmed(frame.type))next.confirmedTransmitted=frame.counter;
    next.cachedTransmitCounter=frame.counter;next.cachedTransmitContext=context;
    next.lastTransmitted=wire; next.transmitCopies=1; next.mayRepeat=role_==Role::Device;
    commit(next); return wire;
}
Bytes Session::repeatUplink(std::optional<DataContext> context) {
    available(busy_,failed_);
    if(role_!=Role::Device || !state_.mayRepeat || state_.lastTransmitted.empty()
       || state_.transmitCopies>=state_.maxUplinkCopies)
        throw Error("No uplink repetition available");
    auto next=state_; ++next.transmitCopies;
    if(state_.keys.version==Version::V1_1 && context) {
        auto frame=decodeData(state_.lastTransmitted,state_.keys,state_.cachedTransmitCounter,state_.cachedTransmitContext);
        context->confirmedCounter=state_.cachedTransmitContext.confirmedCounter;
        next.lastTransmitted=encodeData(frame,state_.keys,*context);next.cachedTransmitContext=*context;
    }
    commit(next); return state_.lastTransmitted;
}
void Session::setNbTrans(std::uint8_t copies) {
    available(busy_,failed_);
    if(copies<1 || copies>15) throw Error("NbTrans must be 1..15");
    auto next=state_; next.maxUplinkCopies=copies; commit(next);
}
DataFrame Session::inspect(const Bytes &wire,DataContext context,std::optional<std::uint32_t> hint) const {
    available(busy_,failed_);
    if(wireAddress(wire)!=state_.devAddr) throw Error("Frame addressed to another device");
    const auto previous=applicationDown(wire,state_.keys)?state_.lastApplicationReceived:state_.lastReceived;
    const auto low=wireCounter(wire);
    std::uint64_t counter=hint?*hint:low;
    if(!hint && previous){counter=(*previous&0xffff0000U)|std::uint32_t(low);if(counter<=*previous)counter+=0x10000;}
    if(counter>0xffffffffULL || (previous && counter<=*previous))throw Error("Replayed or exhausted frame counter");
    auto frame=decodeData(wire,state_.keys,std::uint32_t(counter),contextFor(state_,context,false));
    if(direction(frame.type)==outgoing(role_))throw Error("Wrong incoming frame direction");
    if(state_.keys.version==Version::V1_1 && (frame.control&0x20) && !state_.confirmedTransmitted)
        throw Error("ACK without an outstanding confirmed transmission");
    return frame;
}
Received Session::receive(const Bytes &wire, std::optional<std::uint32_t> hint,DataContext context) {
    available(busy_,failed_);
    if(wireAddress(wire)!=state_.devAddr) throw Error("Frame addressed to another device");
    const auto low=wireCounter(wire);
    const bool sameUplink=state_.lastReceived && role_==Role::Network && !state_.lastUplink.empty() &&
        wire.size()==state_.lastUplink.size() && std::equal(wire.begin(),wire.end()-4,state_.lastUplink.begin());
    if(sameUplink) {
        if(hint && *hint!=*state_.lastReceived) throw Error("Counter hint conflicts with cached uplink");
        if(state_.uplinkCopies>=state_.maxUplinkCopies) throw Error("Uplink replay exceeds NbTrans");
        context.confirmedCounter=state_.cachedUplinkContext.confirmedCounter;
        auto frame=decodeData(wire,state_.keys,*state_.lastReceived,context);
        auto next=state_; ++next.uplinkCopies;
        if(confirmed(frame.type)) next.acknowledgmentPending=true;
        commit(next); frame.payload.clear(); frame.options.clear();
        return {std::move(frame),true}; // Never re-deliver application or MAC data.
    }
    (void)low;
    auto frame=inspect(wire,context,hint);
    auto next=state_;
    if(applicationDown(frame,state_.keys))next.lastApplicationReceived=frame.counter;else next.lastReceived=frame.counter;
    if(confirmed(frame.type)) { next.acknowledgmentPending=true;next.confirmedReceived=frame.counter; }
    if(frame.control&0x20)next.confirmedTransmitted.reset();
    if(role_==Role::Network) { next.lastUplink=wire; next.uplinkCopies=1; next.cachedUplinkContext=contextFor(state_,context,false); }
    else next.mayRepeat=false;
    commit(next); return {std::move(frame),false};
}
OtaaDevice::OtaaDevice(Credentials credentials, DeviceJoinState restored, Persist persist, bool increasing)
    : credentials_(credentials),state_(std::move(restored)),persist_(std::move(persist)),increasing_(increasing) {
    if(!persist_) throw Error("OTAA requires durable persistence");
    if(credentials_.version!=Version::V1_0_4 && credentials_.version!=Version::V1_1)throw Error("Unsupported OTAA version");
    if(credentials_.version==Version::V1_1 && !increasing_)throw Error("LoRaWAN 1.1 requires increasing JoinNonce");
    (void)saveDeviceJoin(state_);
}
Bytes OtaaDevice::request() {
    available(busy_,failed_);
    if(state_.nextDevNonce>0xffff) throw Error("DevNonce exhausted; provision a new JoinEUI/key context");
    const auto nonce=std::uint16_t(state_.nextDevNonce);
    const auto wire=encodeJoinRequest({credentials_.joinEui,credentials_.devEui,nonce},
        credentials_.version==Version::V1_1?credentials_.nwkKey:credentials_.appKey);
    auto next=state_; ++next.nextDevNonce; next.pendingDevNonce=nonce;
    commit(state_,next,persist_,busy_,failed_); return wire;
}
JoinAccept OtaaDevice::accept(const Bytes &wire, const std::function<void(const JoinAccept &)> &validateParameters) {
    available(busy_,failed_);
    if(!state_.pendingDevNonce) throw Error("No pending join request");
    const auto a=credentials_.version==Version::V1_1 ? decodeJoinAccept11(wire,credentials_,*state_.pendingDevNonce):decodeJoinAccept(wire,credentials_.appKey);
    if(state_.lastJoinNonce && (a.joinNonce==*state_.lastJoinNonce || (increasing_ && a.joinNonce<*state_.lastJoinNonce)))
        throw Error("Replayed JoinNonce");
    if(validateParameters) {
        busy_=true;
        try { validateParameters(a); }
        catch(...) { busy_=false; throw; }
        busy_=false;
    }
    auto next=state_; next.lastJoinNonce=a.joinNonce;
    next.activated=activated(a,credentials_,*state_.pendingDevNonce,Role::Device);
    next.pendingDevNonce.reset(); commit(state_,next,persist_,busy_,failed_); return a;
}
void OtaaDevice::updateSession(const SessionState &session) {
    available(busy_,failed_); checkUpdate(state_.activated,session);
    auto next=state_; next.activated=session; commit(state_,next,persist_,busy_,failed_);
}
JoinServer::JoinServer(Credentials credentials, JoinServerState restored, Persist persist)
    : credentials_(credentials),state_(std::move(restored)),persist_(std::move(persist)) {
    if(!persist_) throw Error("Join server requires durable persistence");
    if(credentials_.version!=Version::V1_0_4 && credentials_.version!=Version::V1_1)throw Error("Unsupported join server version");
    (void)saveJoinServer(state_);
}
JoinResponse JoinServer::accept(const Bytes &wire, JoinAccept a) {
    available(busy_,failed_);
    const auto request=decodeJoinRequest(wire,credentials_.version==Version::V1_1?credentials_.nwkKey:credentials_.appKey);
    if(request.joinEui!=credentials_.joinEui || request.devEui!=credentials_.devEui)
        throw Error("Join request identity not provisioned");
    if(state_.lastDevNonce && request.devNonce<=*state_.lastDevNonce) throw Error("Replayed DevNonce");
    if(state_.nextJoinNonce>0xffffff) throw Error("JoinNonce exhausted; provision new root-key context");
    a.joinNonce=state_.nextJoinNonce;
    a.optNeg=credentials_.version==Version::V1_1;
    JoinResponse response{a.optNeg?encodeJoinAccept11(a,credentials_,request.devNonce):encodeJoinAccept(a,credentials_.appKey),a,
                          activated(a,credentials_,request.devNonce,Role::Network)};
    auto next=state_; ++next.nextJoinNonce; next.lastDevNonce=request.devNonce; next.activated=response.session;
    commit(state_,next,persist_,busy_,failed_); return response;
}
void JoinServer::updateSession(const SessionState &session) {
    available(busy_,failed_); checkUpdate(state_.activated,session);
    auto next=state_; next.activated=session; commit(state_,next,persist_,busy_,failed_);
}
}
