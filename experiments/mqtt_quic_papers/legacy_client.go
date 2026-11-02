// Measured pub/sub harness for the restored Fernandez/Jeddou Paho fork.
// The same in-process monotonic clock supplies generation and reception times.
package main

import (
 "crypto/tls"
 "crypto/x509"
 "encoding/binary"
 "encoding/json"
 "flag"
 "fmt"
 "io/ioutil"
 "os"
 "runtime"
 "sort"
 "syscall"
 "time"
 mqtt "github.com/eclipse/paho.mqtt.golang"
)

func checked(token mqtt.Token) { if !token.WaitTimeout(10*time.Second) { panic("MQTT token timeout") }; if token.Error()!=nil { panic(token.Error()) } }
func cpu() float64 { var r syscall.Rusage; if err:=syscall.Getrusage(syscall.RUSAGE_SELF,&r);err!=nil {panic(err)};return float64(r.Utime.Sec+r.Stime.Sec)+float64(r.Utime.Usec+r.Stime.Usec)/1e6 }
func main() {
 broker:=flag.String("broker","","loopback broker URL")
 ca:=flag.String("ca","","experiment CA")
 count:=flag.Int("count",100,"messages")
 size:=flag.Int("size",256,"payload bytes")
 qos:=flag.Int("qos",0,"MQTT QoS")
 interval:=flag.Int("interval-ms",33,"generation interval")
 flag.Parse()
 if *count<1 || *count>10000 || *size<12 || *size>65536 || *qos<0 || *qos>2 {panic("invalid workload")}
 pem,err:=ioutil.ReadFile(*ca);if err!=nil {panic(err)}
 roots:=x509.NewCertPool();if !roots.AppendCertsFromPEM(pem) {panic("invalid CA")}
 config:=&tls.Config{RootCAs:roots,ServerName:"localhost",MinVersion:tls.VersionTLS13,MaxVersion:tls.VersionTLS13}
 start:=time.Now(); cpuStart:=cpu(); var m0,m1 runtime.MemStats;runtime.ReadMemStats(&m0)
 prefix:=fmt.Sprintf("lab-%d",os.Getpid());topic:=prefix+"/frames"
 received:=make(chan struct{seq uint32; ms float64},*count*2)
 opts:=func(id string)*mqtt.ClientOptions{return mqtt.NewClientOptions().AddBroker(*broker).SetClientID(id).SetTLSConfig(config).SetAutoReconnect(false).SetCleanSession(true)}
 sub:=mqtt.NewClient(opts(prefix+"-sub"));checked(sub.Connect())
 checked(sub.Subscribe(topic,byte(*qos),func(_ mqtt.Client,message mqtt.Message){
  data:=message.Payload();if len(data)!=*size {panic("payload size mismatch")}
  seq:=binary.BigEndian.Uint32(data[8:12]);generated:=binary.BigEndian.Uint64(data[:8])
  if int(seq)>=*count {panic("invalid sequence")}
  received<-struct{seq uint32;ms float64}{seq,float64(time.Since(start).Nanoseconds()-int64(generated))/1e6}
 }))
 connectStart:=time.Now();pub:=mqtt.NewClient(opts(prefix+"-pub"));checked(pub.Connect());connack:=time.Since(connectStart).Seconds()*1000
 first:=time.Now()
 for i:=0;i<*count;i++ {
  data:=make([]byte,*size);binary.BigEndian.PutUint64(data[:8],uint64(time.Since(start).Nanoseconds()));binary.BigEndian.PutUint32(data[8:12],uint32(i))
  checked(pub.Publish(topic,byte(*qos),false,data))
  if i+1<*count { if wait:=time.Until(first.Add(time.Duration(i+1)*time.Duration(*interval)*time.Millisecond));wait>0 {time.Sleep(wait)} }
 }
 seen:=map[uint32]bool{};samples:=make([]float64,0,*count);duplicates:=0
 timeout:=time.NewTimer(10*time.Second);defer timeout.Stop()
 for len(seen)<*count {select {case r:=<-received:if seen[r.seq] {duplicates++;continue};seen[r.seq]=true;samples=append(samples,r.ms)
 case <-timeout.C:panic(fmt.Sprintf("only %d/%d received",len(seen),*count))}}
 execution:=time.Since(connectStart).Seconds();runtime.ReadMemStats(&m1);cpuElapsed:=cpu()-cpuStart
 pub.Disconnect(100);sub.Disconnect(100)
 sort.Float64s(samples);mean:=0.0;for _,v:=range samples {mean+=v};mean/=float64(len(samples))
 result:=map[string]interface{}{"sent":*count,"received_unique":len(seen),"duplicates":duplicates,"payload_bytes":*size,"qos":*qos,
  "latency_samples_ms":samples,"latency_mean_ms":mean,"publisher_execution_s":execution,"connect_to_connack_ms":connack,
  "cpu_process_s":cpuElapsed,"go_total_alloc_delta_bytes":m1.TotalAlloc-m0.TotalAlloc,"scope":"publisher+subscriber process; broker excluded; start before subscriber connect, end before disconnect","tls_verification":true}
 data,err:=json.Marshal(result);if err!=nil {panic(err)};fmt.Println(string(data))
}
