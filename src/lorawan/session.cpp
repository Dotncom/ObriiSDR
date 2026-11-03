#include "lorawan.h"
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
SessionState activated(const JoinAccept &a, const Key &key, std::uint16_t nonce, Role role) {
    SessionState session; session.role=role; session.devAddr=a.devAddr;
    session.keys=deriveSessionKeys(key,a.joinNonce,a.netId,nonce); return session;
}
Direction outgoing(Role role) { return role==Role::Device ? Direction::Uplink:Direction::Downlink; }
void checkUpdate(const std::optional<SessionState> &previous, const SessionState &next) {
    (void)saveSession(next);
    if(!previous || previous->role!=next.role || previous->devAddr!=next.devAddr
       || previous->keys.network!=next.keys.network || previous->keys.application!=next.keys.application
       || next.nextTransmit<previous->nextTransmit
       || (previous->lastReceived && (!next.lastReceived || *next.lastReceived<*previous->lastReceived)))
        throw Error("Session update would change key context or roll back counters");
    if(next.nextTransmit==previous->nextTransmit
       && (next.lastTransmitted!=previous->lastTransmitted || next.transmitCopies<previous->transmitCopies
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
Bytes Session::transmit(DataFrame frame) {
    available(busy_,failed_);
    if(direction(frame.type)!=outgoing(role_)) throw Error("Wrong outgoing frame direction");
    if(state_.nextTransmit>0xffffffffULL) throw Error("Frame counter exhausted; activate with new session keys");
    frame.devAddr=state_.devAddr;
    frame.counter=std::uint32_t(state_.nextTransmit);
    if(state_.acknowledgmentPending) frame.control|=0x20;
    const auto wire=encodeData(frame,state_.keys);
    auto next=state_; ++next.nextTransmit; next.acknowledgmentPending=false;
    next.lastTransmitted=wire; next.transmitCopies=1; next.mayRepeat=role_==Role::Device;
    commit(next); return wire;
}
Bytes Session::repeatUplink() {
    available(busy_,failed_);
    if(role_!=Role::Device || !state_.mayRepeat || state_.lastTransmitted.empty()
       || state_.transmitCopies>=state_.maxUplinkCopies)
        throw Error("No uplink repetition available");
    auto next=state_; ++next.transmitCopies; commit(next); return state_.lastTransmitted;
}
void Session::setNbTrans(std::uint8_t copies) {
    available(busy_,failed_);
    if(copies<1 || copies>15) throw Error("NbTrans must be 1..15");
    auto next=state_; next.maxUplinkCopies=copies; commit(next);
}
Received Session::receive(const Bytes &wire, std::optional<std::uint32_t> hint) {
    available(busy_,failed_);
    if(wireAddress(wire)!=state_.devAddr) throw Error("Frame addressed to another device");
    const auto low=wireCounter(wire);
    if(state_.lastReceived && role_==Role::Network && wire==state_.lastUplink) {
        if(hint && *hint!=*state_.lastReceived) throw Error("Counter hint conflicts with cached uplink");
        if(state_.uplinkCopies>=state_.maxUplinkCopies) throw Error("Uplink replay exceeds NbTrans");
        auto frame=decodeData(wire,state_.keys,*state_.lastReceived);
        auto next=state_; ++next.uplinkCopies;
        if(confirmed(frame.type)) next.acknowledgmentPending=true;
        commit(next); frame.payload.clear(); frame.options.clear();
        return {std::move(frame),true}; // Never re-deliver application or MAC data.
    }
    std::uint64_t counter=hint ? *hint:low;
    if(!hint && state_.lastReceived) {
        counter=(*state_.lastReceived&0xffff0000U)|std::uint32_t(low);
        if(counter<=*state_.lastReceived) counter+=0x10000;
    }
    if(counter>0xffffffffULL || (state_.lastReceived && counter<=*state_.lastReceived))
        throw Error("Replayed or exhausted frame counter");
    auto frame=decodeData(wire,state_.keys,std::uint32_t(counter));
    if(direction(frame.type)==outgoing(role_)) throw Error("Wrong incoming frame direction");
    auto next=state_; next.lastReceived=std::uint32_t(counter);
    if(confirmed(frame.type)) next.acknowledgmentPending=true;
    if(role_==Role::Network) { next.lastUplink=wire; next.uplinkCopies=1; }
    else next.mayRepeat=false;
    commit(next); return {std::move(frame),false};
}
OtaaDevice::OtaaDevice(Credentials credentials, DeviceJoinState restored, Persist persist, bool increasing)
    : credentials_(credentials),state_(std::move(restored)),persist_(std::move(persist)),increasing_(increasing) {
    if(!persist_) throw Error("OTAA requires durable persistence");
    (void)saveDeviceJoin(state_);
}
Bytes OtaaDevice::request() {
    available(busy_,failed_);
    if(state_.nextDevNonce>0xffff) throw Error("DevNonce exhausted; provision a new JoinEUI/key context");
    const auto nonce=std::uint16_t(state_.nextDevNonce);
    const auto wire=encodeJoinRequest({credentials_.joinEui,credentials_.devEui,nonce},credentials_.appKey);
    auto next=state_; ++next.nextDevNonce; next.pendingDevNonce=nonce;
    commit(state_,next,persist_,busy_,failed_); return wire;
}
JoinAccept OtaaDevice::accept(const Bytes &wire, const std::function<void(const JoinAccept &)> &validateParameters) {
    available(busy_,failed_);
    if(!state_.pendingDevNonce) throw Error("No pending join request");
    const auto a=decodeJoinAccept(wire,credentials_.appKey);
    if(state_.lastJoinNonce && (a.joinNonce==*state_.lastJoinNonce || (increasing_ && a.joinNonce<*state_.lastJoinNonce)))
        throw Error("Replayed JoinNonce");
    if(validateParameters) {
        busy_=true;
        try { validateParameters(a); }
        catch(...) { busy_=false; throw; }
        busy_=false;
    }
    auto next=state_; next.lastJoinNonce=a.joinNonce;
    next.activated=activated(a,credentials_.appKey,*state_.pendingDevNonce,Role::Device);
    next.pendingDevNonce.reset(); commit(state_,next,persist_,busy_,failed_); return a;
}
void OtaaDevice::updateSession(const SessionState &session) {
    available(busy_,failed_); checkUpdate(state_.activated,session);
    auto next=state_; next.activated=session; commit(state_,next,persist_,busy_,failed_);
}
JoinServer::JoinServer(Credentials credentials, JoinServerState restored, Persist persist)
    : credentials_(credentials),state_(std::move(restored)),persist_(std::move(persist)) {
    if(!persist_) throw Error("Join server requires durable persistence");
    (void)saveJoinServer(state_);
}
JoinResponse JoinServer::accept(const Bytes &wire, JoinAccept a) {
    available(busy_,failed_);
    const auto request=decodeJoinRequest(wire,credentials_.appKey);
    if(request.joinEui!=credentials_.joinEui || request.devEui!=credentials_.devEui)
        throw Error("Join request identity not provisioned");
    if(state_.lastDevNonce && request.devNonce<=*state_.lastDevNonce) throw Error("Replayed DevNonce");
    if(state_.nextJoinNonce>0xffffff) throw Error("JoinNonce exhausted; provision new root-key context");
    a.joinNonce=state_.nextJoinNonce;
    JoinResponse response{encodeJoinAccept(a,credentials_.appKey),a,
                          activated(a,credentials_.appKey,request.devNonce,Role::Network)};
    auto next=state_; ++next.nextJoinNonce; next.lastDevNonce=request.devNonce; next.activated=response.session;
    commit(state_,next,persist_,busy_,failed_); return response;
}
void JoinServer::updateSession(const SessionState &session) {
    available(busy_,failed_); checkUpdate(state_.activated,session);
    auto next=state_; next.activated=session; commit(state_,next,persist_,busy_,failed_);
}
}
