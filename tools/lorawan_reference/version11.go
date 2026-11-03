// Independent air-format fixtures. Production C++ is never linked here.
package main

import (
    "bufio"
    "crypto/aes"
    "encoding/binary"
    "fmt"
    "os"
    lw "github.com/brocaar/lorawan"
)
func derive11(key lw.AES128Key, kind byte, nonce uint32, eui lw.EUI64, dev uint16) lw.AES128Key {
    var a,b [16]byte;a[0]=kind
    binary.LittleEndian.PutUint32(a[1:5],nonce)
    for i:=0;i<8;i++ { a[4+i]=eui[7-i] }
    binary.LittleEndian.PutUint16(a[12:14],dev)
    cipher,err:=aes.NewCipher(key[:]);must(err);cipher.Encrypt(b[:],a[:]);return lw.AES128Key(b)
}
func deriveJS(key lw.AES128Key, kind byte, dev lw.EUI64) lw.AES128Key {
    var a,b [16]byte;a[0]=kind;for i:=0;i<8;i++ {a[1+i]=dev[7-i]}
    cipher,err:=aes.NewCipher(key[:]);must(err);cipher.Encrypt(b[:],a[:]);return lw.AES128Key(b)
}
func vectors11(path string) {
    f,err:=os.Create(path);must(err);w:=bufio.NewWriter(f)
    fmt.Fprintln(w,"# brocaar/lorawan 4bb09b3bdacba097680894e8d35dbcbc0d260c7e LoRaWAN1_1; FOpts counter-domain erratum")
    var fk,sk,ek,ak lw.AES128Key
    for i:=range fk {fk[i]=byte(i+1);sk[i]=byte(i+33);ek[i]=byte(i+65);ak[i]=byte(i+97)}
    addr:=lw.DevAddr{0x26,1,0x1b,0xda};count:=0
    for _,typ:=range []lw.MType{lw.UnconfirmedDataUp,lw.UnconfirmedDataDown,lw.ConfirmedDataUp,lw.ConfirmedDataDown} {
      up:=typ==lw.UnconfirmedDataUp||typ==lw.ConfirmedDataUp
      for _,counter:=range []uint32{0,1,65535,65536,0x12345678,0xffffffff} {
       for _,opt:=range []int{0,3,15} {
        for _,port:=range []int{-1,0,1,223} {
         if port==0&&opt>0 {continue}
         for _,size:=range []int{0,1,16,51,115,242} {
          if (port<0&&size>0)||12+opt+1+size>255 {continue}
          for _,ack:=range []bool{false,true} {
           options:=make([]byte,opt);for i:=range options{options[i]=byte(i+2)}
           payload:=make([]byte,size);for i:=range payload{payload[i]=byte(i*17+9)}
           mac:=&lw.MACPayload{FHDR:lw.FHDR{DevAddr:addr,FCnt:counter,FCtrl:lw.FCtrl{ADR:true,ACK:ack}}}
           if opt>0 {mac.FHDR.FOpts=[]lw.Payload{&lw.DataPayload{Bytes:append([]byte{},options...)}}}
           pt:="-";if port>=0 {p:=uint8(port);mac.FPort=&p;pt=fmt.Sprint(port);mac.FRMPayload=[]lw.Payload{&lw.DataPayload{Bytes:payload}}}
           phy:=lw.PHYPayload{MHDR:lw.MHDR{MType:typ,Major:lw.LoRaWANR1},MACPayload:mac}
           must(phy.EncryptFOpts(ek))
           if port>=0 {if port==0 {must(phy.EncryptFRMPayload(ek))} else {must(phy.EncryptFRMPayload(ak))}}
           dr,ch,conf:=uint8(counter%7),uint8(counter%16),uint32(0xabcd)
           if up {must(phy.SetUplinkDataMIC(lw.LoRaWAN1_1,conf,dr,ch,fk,sk))} else {must(phy.SetDownlinkDataMIC(lw.LoRaWAN1_1,conf,sk))}
           raw,err:=phy.MarshalBinary();must(err);flags:=byte(0x80);if ack{flags|=0x20}
           fmt.Fprintf(w,"data11 %x %x %x %x %d %x %d %02x %s %s %s %d %d %d\t%x\n",fk[:],sk[:],ek[:],ak[:],typ,addr[:],counter,flags,text(options),pt,text(payload),dr,ch,conf,raw);count++
          }
         }
        }
       }
      }
    }
    join,dev:=lw.EUI64{1,2,3,4,5,6,7,8},lw.EUI64{0x80,0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0}
    for _,nonce:=range []uint16{0,1,255,65535} {
      jsEnc,jsInt:=deriveJS(fk,5,dev),deriveJS(fk,6,dev)
      fmt.Fprintf(w,"js11 %x %x\t%x %x\n",fk[:],dev[:],jsEnc[:],jsInt[:]);count++
      for _,jn:=range []uint32{0,1,0xffffff} {
        a,b,c,d:=derive11(fk,1,jn,join,nonce),derive11(fk,3,jn,join,nonce),derive11(fk,4,jn,join,nonce),derive11(ak,2,jn,join,nonce)
        fmt.Fprintf(w,"derive11 %x %x %d %x %d\t%x %x %x %x\n",fk[:],ak[:],jn,join[:],nonce,a[:],b[:],c[:],d[:]);count++
        for _,typ:=range []lw.JoinType{lw.JoinRequestType,0,1,2} {
          accept:=&lw.JoinAcceptPayload{JoinNonce:lw.JoinNonce(jn),HomeNetID:lw.NetID{0,0,0x13},DevAddr:addr,DLSettings:lw.DLSettings{OptNeg:true,RX1DROffset:2,RX2DataRate:3},RXDelay:5}
          phy:=lw.PHYPayload{MHDR:lw.MHDR{MType:lw.JoinAccept,Major:lw.LoRaWANR1},MACPayload:accept}
          must(phy.SetDownlinkJoinMIC(typ,join,lw.DevNonce(nonce),jsInt));enc:=fk;if typ!=lw.JoinRequestType{enc=jsEnc};must(phy.EncryptJoinAcceptPayload(enc))
          raw,err:=phy.MarshalBinary();must(err)
          fmt.Fprintf(w,"accept11 %x %x %x %x %d %d %d\t%x\n",fk[:],ak[:],join[:],dev[:],nonce,jn,typ,raw);count++
        }
      }
      for _,typ:=range []lw.JoinType{0,1,2} {
        phy:=lw.PHYPayload{MHDR:lw.MHDR{MType:lw.RejoinRequest,Major:lw.LoRaWANR1}}
        key:=sk
        if typ==1 {phy.MACPayload=&lw.RejoinRequestType1Payload{RejoinType:typ,JoinEUI:join,DevEUI:dev,RJCount1:nonce};key=jsInt} else {
          phy.MACPayload=&lw.RejoinRequestType02Payload{RejoinType:typ,NetID:lw.NetID{0,0,0x13},DevEUI:dev,RJCount0:nonce}
        }
        must(phy.SetUplinkJoinMIC(key));raw,err:=phy.MarshalBinary();must(err)
        fmt.Fprintf(w,"rejoin11 %x %x %x %d %d\t%x\n",key[:],join[:],dev[:],nonce,typ,raw);count++
      }
    }
    // AES slot randomisation uses the standard library, independently of C++.
    cipher,err:=aes.NewCipher(make([]byte,16));must(err)
    for _,time:=range []uint32{0,128,0xcc020000,0xffffff80} {for p:=0;p<8;p++ {
      var in,out [16]byte;binary.LittleEndian.PutUint32(in[:4],time);binary.LittleEndian.PutUint32(in[4:8],0x26011bda);cipher.Encrypt(out[:],in[:])
      offset:=binary.LittleEndian.Uint16(out[:2])%uint16(32<<p)
      fmt.Fprintf(w,"ping %d 26011bda %d\t%d\n",time,p,offset);count++
    }}
    must(w.Flush());must(f.Close());fmt.Printf("Generated %d independent 1.1/Class B vectors\n",count)
}
