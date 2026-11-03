#include "meshtastic.h"
#include "meshprotobuf_internal.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace obrii::meshtastic {
namespace {
struct Field {
    unsigned number=0,wire=0;std::uint64_t value=0;
    std::size_t begin=0,end=0,data=0,size=0;
};
class Reader {
public:
    explicit Reader(const Bytes &b):bytes(b) {
        if(b.size()>DataMtu)throw std::invalid_argument("Protobuf exceeds radio data MTU");
    }
    bool next(Field &f) {
        if(at==bytes.size())return false;
        f={};f.begin=at;
        const auto key=varint();
        if(!(key>>3)||(key>>3)>0x1fffffff)throw std::invalid_argument("Invalid protobuf field tag");
        f.number=unsigned(key>>3);f.wire=unsigned(key&7);
        if(f.wire==0)f.value=varint();
        else if(f.wire==1||f.wire==5) {
            const auto n=f.wire==1?8u:4u;require(n);f.data=at;f.size=n;
            for(unsigned i=0;i<n;++i)f.value|=std::uint64_t(bytes[at++])<<(8*i);
        } else if(f.wire==2) {
            const auto size=varint();
            if(size>bytes.size()-at)throw std::invalid_argument("Truncated protobuf bytes");
            f.data=at;f.size=std::size_t(size);at+=f.size;
        } else throw std::invalid_argument("Unsupported protobuf wire type");
        f.end=at;return true;
    }
private:
    void require(std::size_t count) { if(count>bytes.size()-at)throw std::invalid_argument("Truncated protobuf field"); }
    std::uint64_t varint() {
        std::uint64_t value=0;
        for(unsigned i=0;i<10;++i) {
            require(1);const auto b=bytes[at++];
            if(i==9&&b>1)throw std::invalid_argument("Protobuf varint overflow");
            value|=std::uint64_t(b&127)<<(7*i);
            if(!(b&128))return value;
        }
        throw std::invalid_argument("Protobuf varint overflow");
    }
    const Bytes &bytes;std::size_t at=0;
};
void type(const Field &f,unsigned expected) {
    if(f.wire!=expected)throw std::invalid_argument("Wrong protobuf field type");
}
std::uint32_t u32(const Field &f,unsigned wire) {
    type(f,wire);if(f.value>0xffffffffu)throw std::invalid_argument("Protobuf uint32 overflow");
    return std::uint32_t(f.value);
}
Bytes content(const Bytes &b,const Field &f) {
    type(f,2);return Bytes(b.begin()+f.data,b.begin()+f.data+f.size);
}
std::string string(const Bytes &b,const Field &f) {
    const auto data=content(b,f);std::string out(data.begin(),data.end());
    if(!detail::validUtf8(out))throw std::invalid_argument("Invalid protobuf UTF-8");return out;
}
void unknown(Bytes &out,const Bytes &b,const Field &f) { out.insert(out.end(),b.begin()+f.begin,b.begin()+f.end); }
void varint(Bytes &b,std::uint64_t value) {
    while(value>127) { b.push_back(std::uint8_t(value)|128);value>>=7; }
    b.push_back(std::uint8_t(value));
}
void number(Bytes &b,unsigned tag,std::uint64_t value,bool force=false) {
    if(!value&&!force)return;varint(b,std::uint64_t(tag)<<3);varint(b,value);
}
void fixed(Bytes &b,unsigned tag,std::uint32_t value,bool force=false) {
    if(!value&&!force)return;varint(b,(std::uint64_t(tag)<<3)|5);
    for(int i=0;i<4;++i)b.push_back(std::uint8_t(value>>(8*i)));
}
void blob(Bytes &b,unsigned tag,const Bytes &value,bool force=false) {
    if(value.empty()&&!force)return;varint(b,(std::uint64_t(tag)<<3)|2);varint(b,value.size());
    b.insert(b.end(),value.begin(),value.end());
}
void string(Bytes &b,unsigned tag,const std::string &value) {
    if(!detail::validUtf8(value)||value.find('\0')!=std::string::npos)throw std::invalid_argument("Invalid UTF-8 string");
    blob(b,tag,Bytes(value.begin(),value.end()));
}
Bytes finish(Bytes b,const Bytes &extra,std::initializer_list<unsigned> known) {
    Reader reader(extra);Field f;
    while(reader.next(f)) for(auto tag:known) if(f.number==tag)
        throw std::invalid_argument("Unknown protobuf fields overlap known fields");
    b.insert(b.end(),extra.begin(),extra.end());
    if(b.size()>DataMtu)throw std::invalid_argument("Encoded protobuf exceeds radio data MTU");
    return b;
}
std::int32_t signed32(std::uint32_t bits) { std::int32_t value;std::memcpy(&value,&bits,4);return value; }
void validatePosition(const Position &p) {
    if(p.latitudeE7&&(*p.latitudeE7 < -900000000||*p.latitudeE7 > 900000000))throw std::invalid_argument("Invalid latitude");
    if(p.longitudeE7&&(*p.longitudeE7 < -1800000000||*p.longitudeE7 > 1800000000))throw std::invalid_argument("Invalid longitude");
}
}
namespace detail {
std::vector<ProtoField> protoFields(const Bytes &bytes) {
    std::vector<ProtoField> fields;Reader reader(bytes);Field f;
    while(reader.next(f))fields.push_back({f.number,f.wire,f.value,f.wire==2?content(bytes,f):Bytes{}});
    return fields;
}
Bytes protoEncode(const std::vector<ProtoField> &fields) {
    Bytes b;
    for(const auto &f:fields) {
        if(!f.tag||f.tag>0x1fffffff)throw std::invalid_argument("Invalid protobuf tag");
        if(f.wire==0)number(b,f.tag,f.value,true);
        else if(f.wire==2)blob(b,f.tag,f.bytes,true);
        else if(f.wire==5&&f.value<=0xffffffffu)fixed(b,f.tag,std::uint32_t(f.value),true);
        else if(f.wire==1) { varint(b,(std::uint64_t(f.tag)<<3)|1);for(int i=0;i<8;++i)b.push_back(std::uint8_t(f.value>>(8*i))); }
        else throw std::invalid_argument("Invalid protobuf wire type/value");
        if(b.size()>DataMtu)throw std::length_error("Protobuf exceeds radio MTU");
    }
    return b;
}
bool validUtf8(const std::string &text) {
    for(std::size_t i=0;i<text.size();) {
        unsigned c=static_cast<unsigned char>(text[i++]);
        if(c<128)continue;
        int count=0;unsigned min=0;
        if(c>=0xc2&&c<=0xdf) { count=1;c&=31;min=128; }
        else if(c>=0xe0&&c<=0xef) { count=2;c&=15;min=2048; }
        else if(c>=0xf0&&c<=0xf4) { count=3;c&=7;min=65536; }
        else return false;
        if(text.size()-i<std::size_t(count))return false;
        for(int j=0;j<count;++j) { const auto b=static_cast<unsigned char>(text[i++]);if((b&0xc0)!=0x80)return false;c=(c<<6)|(b&63); }
        if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
    }
    return true;
}
}
Bytes encodeData(const Data &d) {
    Bytes b;number(b,1,d.port);blob(b,2,d.payload);number(b,3,d.wantResponse);
    fixed(b,4,d.dest);fixed(b,5,d.source);fixed(b,6,d.requestId);fixed(b,7,d.replyId);fixed(b,8,d.emoji);
    if(d.bitfield)number(b,9,*d.bitfield,true);blob(b,10,d.signature);
    return finish(std::move(b),d.unknownFields,{1,2,3,4,5,6,7,8,9,10});
}
Data decodeData(const Bytes &bytes) {
    Data d;Reader r(bytes);Field f;
    while(r.next(f))switch(f.number) {
    case 1:d.port=u32(f,0);break;
    case 2:d.payload=content(bytes,f);break;
    case 3:type(f,0);d.wantResponse=f.value!=0;break;
    case 4:d.dest=u32(f,5);break;
    case 5:d.source=u32(f,5);break;
    case 6:d.requestId=u32(f,5);break;
    case 7:d.replyId=u32(f,5);break;
    case 8:d.emoji=u32(f,5);break;
    case 9:d.bitfield=u32(f,0);break;
    case 10:d.signature=content(bytes,f);break;
    default:unknown(d.unknownFields,bytes,f);
    }
    return d;
}
Data textMessage(const std::string &utf8) {
    if(!detail::validUtf8(utf8))throw std::invalid_argument("Invalid text UTF-8");
    Data d;d.port=TextPort;d.payload.assign(utf8.begin(),utf8.end());encodeData(d);return d;
}
std::string text(const Data &d) {
    if(d.port!=TextPort)throw std::invalid_argument("Not a text message");
    std::string result(d.payload.begin(),d.payload.end());
    if(!detail::validUtf8(result))throw std::invalid_argument("Invalid received text UTF-8");
    return result;
}
Bytes encodeUser(const User &u) {
    if(u.id.size()>32||u.longName.size()>24||u.shortName.size()>4||u.macAddress.size()>6||(!u.publicKey.empty()&&u.publicKey.size()!=32))
        throw std::invalid_argument("User fields exceed Meshtastic limits");
    Bytes b;string(b,1,u.id);string(b,2,u.longName);string(b,3,u.shortName);blob(b,4,u.macAddress);
    number(b,5,u.hardwareModel);number(b,6,u.licensed);number(b,7,u.role);blob(b,8,u.publicKey);
    if(u.unmessagable)number(b,9,*u.unmessagable,true);
    return finish(std::move(b),u.unknownFields,{1,2,3,4,5,6,7,8,9});
}
User decodeUser(const Bytes &bytes) {
    User u;Reader r(bytes);Field f;
    while(r.next(f))switch(f.number) {
    case 1:u.id=string(bytes,f);break;
    case 2:u.longName=string(bytes,f);break;
    case 3:u.shortName=string(bytes,f);break;
    case 4:u.macAddress=content(bytes,f);break;
    case 5:u.hardwareModel=u32(f,0);break;
    case 6:type(f,0);u.licensed=f.value!=0;break;
    case 7:u.role=u32(f,0);break;
    case 8:u.publicKey=content(bytes,f);break;
    case 9:type(f,0);u.unmessagable=f.value!=0;break;
    default:unknown(u.unknownFields,bytes,f);
    }
    if(u.id.size()>32||u.longName.size()>39||u.shortName.size()>8||u.macAddress.size()>6||(!u.publicKey.empty()&&u.publicKey.size()!=32))
        throw std::invalid_argument("Invalid received User field lengths");
    return u;
}
Bytes encodePosition(const Position &p) {
    validatePosition(p);Bytes b;
    if(p.latitudeE7)fixed(b,1,std::uint32_t(*p.latitudeE7),true);
    if(p.longitudeE7)fixed(b,2,std::uint32_t(*p.longitudeE7),true);
    if(p.altitudeMeters)number(b,3,std::uint64_t(std::int64_t(*p.altitudeMeters)),true);
    fixed(b,4,p.time);
    return finish(std::move(b),p.unknownFields,{1,2,3,4});
}
Position decodePosition(const Bytes &bytes) {
    Position p;Reader r(bytes);Field f;
    while(r.next(f))switch(f.number) {
    case 1:p.latitudeE7=signed32(u32(f,5));break;
    case 2:p.longitudeE7=signed32(u32(f,5));break;
    case 3:type(f,0);p.altitudeMeters=signed32(std::uint32_t(f.value));break;
    case 4:p.time=u32(f,5);break;
    default:unknown(p.unknownFields,bytes,f);
    }
    validatePosition(p);return p;
}
Bytes encodeRouting(const Routing &r) {
    Bytes b;
    if(r.kind==Routing::Kind::Request)blob(b,1,r.route,true);
    else if(r.kind==Routing::Kind::Reply)blob(b,2,r.route,true);
    else if(r.kind==Routing::Kind::Error)number(b,3,r.error,true);
    blob(b,4,r.ackProof);
    return finish(std::move(b),r.unknownFields,{1,2,3,4});
}
Routing decodeRouting(const Bytes &bytes) {
    Routing value;Reader r(bytes);Field f;
    while(r.next(f))switch(f.number) {
    case 1:value.kind=Routing::Kind::Request;value.route=content(bytes,f);value.error=0;break;
    case 2:value.kind=Routing::Kind::Reply;value.route=content(bytes,f);value.error=0;break;
    case 3:value.kind=Routing::Kind::Error;value.error=u32(f,0);value.route.clear();break;
    case 4:value.ackProof=content(bytes,f);break;
    default:unknown(value.unknownFields,bytes,f);
    }
    return value;
}
}
