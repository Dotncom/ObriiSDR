// Test-data generator using the independent MIT-licensed brocaar/lorawan stack.
package main

import (
    "bufio"
    "crypto/aes"
    "encoding/binary"
    "encoding/hex"
    "flag"
    "fmt"
    "os"
    lw "github.com/brocaar/lorawan"
)

func must(err error) { if err != nil { panic(err) } }
func text(b []byte) string { if len(b)==0 { return "-" }; return hex.EncodeToString(b) }
func main() {
    output:=flag.String("output","vectors.tsv","Output test vectors")
    flag.Parse()
    file,err:=os.Create(*output); must(err)
    writer:=bufio.NewWriter(file)
    fmt.Fprintln(writer,"# brocaar/lorawan commit 4bb09b3bdacba097680894e8d35dbcbc0d260c7e; LoRaWAN1_0")
    var nk,ak lw.AES128Key
    for i:=range nk { nk[i]=byte(0x31+i); ak[i]=byte(0x91+i) }
    addr:=lw.DevAddr{0x26,0x01,0x1b,0xda}
    count:=0
    for _,typ:=range []lw.MType{lw.UnconfirmedDataUp,lw.UnconfirmedDataDown,lw.ConfirmedDataUp,lw.ConfirmedDataDown} {
        uplink:=typ==lw.UnconfirmedDataUp || typ==lw.ConfirmedDataUp
        for _,counter:=range []uint32{0,1,65535,65536,65537,0x12345678,0xffffffff} {
            for _,optionCount:=range []int{0,3,15} {
                opts:=make([]byte,optionCount)
                for i:=range opts { opts[i]=byte(2+i%3) }
                for _,portValue:=range []int{-1,0,1,223,224,255} {
                    if portValue==0 && optionCount>0 { continue }
                    for _,size:=range []int{0,1,15,16,17,51,115,242} {
                        if (portValue<0 && size>0) || 12+optionCount+size+1>255 { continue }
                        body:=make([]byte,size); for i:=range body { body[i]=byte(17+i*31) }
                        ctrl:=lw.FCtrl{ADR:true,ACK:counter%2==1,FPending:!uplink}
                        mac:=&lw.MACPayload{FHDR:lw.FHDR{DevAddr:addr,FCtrl:ctrl,FCnt:counter}}
                        if optionCount>0 { mac.FHDR.FOpts=[]lw.Payload{&lw.DataPayload{Bytes:opts}} }
                        portText:="-"
                        if portValue>=0 {
                            port:=uint8(portValue); mac.FPort=&port; portText=fmt.Sprint(portValue)
                            mac.FRMPayload=[]lw.Payload{&lw.DataPayload{Bytes:body}}
                        }
                        phy:=lw.PHYPayload{MHDR:lw.MHDR{MType:typ,Major:lw.LoRaWANR1},MACPayload:mac}
                        if portValue>=0 { if portValue==0 { must(phy.EncryptFRMPayload(nk)) } else { must(phy.EncryptFRMPayload(ak)) } }
                        if uplink { must(phy.SetUplinkDataMIC(lw.LoRaWAN1_0,0,0,0,nk,nk)) } else { must(phy.SetDownlinkDataMIC(lw.LoRaWAN1_0,0,nk)) }
                        wire,err:=phy.MarshalBinary(); must(err)
                        flags:=byte(0x80); if counter%2==1 { flags|=0x20 }; if !uplink { flags|=0x10 }
                        fmt.Fprintf(writer,"data-encode %x %x %d %x %d %02x %s %s %s\t%x\n",nk[:],ak[:],typ,addr[:],counter,flags,text(opts),portText,text(body),wire)
                        count++
                    }
                }
            }
        }
    }
    root:=lw.AES128Key{0,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff}
    joinEui:=lw.EUI64{1,2,3,4,5,6,7,8}; devEui:=lw.EUI64{0x80,0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0}
    for _,nonce:=range []uint16{0,1,255,256,65535} {
        phy:=lw.PHYPayload{MHDR:lw.MHDR{MType:lw.JoinRequest,Major:lw.LoRaWANR1},MACPayload:&lw.JoinRequestPayload{JoinEUI:joinEui,DevEUI:devEui,DevNonce:lw.DevNonce(nonce)}}
        must(phy.SetUplinkJoinMIC(root)); wire,err:=phy.MarshalBinary(); must(err)
        fmt.Fprintf(writer,"join-request %x %x %x %d\t%x\n",root[:],joinEui[:],devEui[:],nonce,wire); count++
        for _,jn:=range []uint32{0,1,0xabcdef,0xffffff} {
            for _,withCF:=range []bool{false,true} {
                accept:=&lw.JoinAcceptPayload{JoinNonce:lw.JoinNonce(jn),HomeNetID:lw.NetID{0x13,0,0x99},DevAddr:addr,
                    DLSettings:lw.DLSettings{RX1DROffset:2,RX2DataRate:3},RXDelay:5}
                cfText:="-"
                if withCF {
                    cf:=&lw.CFList{CFListType:lw.CFListChannel,Payload:&lw.CFListChannelPayload{Channels:[5]uint32{867100000,867300000,867500000,867700000,867900000}}}
                    accept.CFList=cf; raw,err:=cf.MarshalBinary(); must(err); cfText=hex.EncodeToString(raw)
                }
                ap:=lw.PHYPayload{MHDR:lw.MHDR{MType:lw.JoinAccept,Major:lw.LoRaWANR1},MACPayload:accept}
                must(ap.SetDownlinkJoinMIC(lw.JoinRequestType,joinEui,lw.DevNonce(nonce),root)); must(ap.EncryptJoinAcceptPayload(root))
                encoded,err:=ap.MarshalBinary(); must(err)
                fmt.Fprintf(writer,"join-accept %x %d 130099 %x 2 3 5 %s\t%x\n",root[:],jn,addr[:],cfText,encoded); count++
            }
            cipher,err:=aes.NewCipher(root[:]); must(err)
            var a,b [16]byte; a[0]=1
            binary.LittleEndian.PutUint32(a[1:5],jn); a[4]=0x99; a[5]=0; a[6]=0x13
            binary.LittleEndian.PutUint16(a[7:9],nonce); cipher.Encrypt(b[:],a[:]); network:=b
            a[0]=2; cipher.Encrypt(b[:],a[:])
            fmt.Fprintf(writer,"derive %x %d 130099 %d\t%x %x\n",root[:],jn,nonce,network[:],b[:]); count++
        }
    }
    must(writer.Flush()); must(file.Close())
    fmt.Printf("Generated %d independent LoRaWAN vectors\n",count)
}
