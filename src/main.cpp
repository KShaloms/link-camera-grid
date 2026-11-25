// 읽는 순서: setup()에서 장치를 준비하고 loop()를 반복합니다.
// loop() → measure() → grid.h의 score()와 Cell::update() 순으로 따라가면 블록 판정 과정을 볼 수 있습니다.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ModbusRTU.h>
#include "img_converters.h"
#include "camera_setup.h"
#include "wifi_config.h"
#include "grid.h"
#include "page.h"

using namespace grid;
WebServer web(80);
// 플래시에 금속 보관판 모서리 네 점을 보관합니다. 빈 판 색상 기준은 아래 background 배열(RAM)에 따로 둡니다.
Preferences prefs;
ModbusRTU mb;
constexpr uint8_t SlaveId=7;
// PLC 통신 주소는 0부터 셉니다. 예: FirstCell=9는 홀딩 레지스터 40010(1번 칸)입니다.
// 오프셋 8은 빈 판 학습 명령, 9~17은 1~9번 유무, 18은 상태, 19는 프레임 번호입니다.
constexpr int Command=8, FirstCell=9, Status=18, Sequence=19;
// 앞에서부터 값 0~4: 기준 대기 → 학습 중 → 판정 준비 → 정상, 또는 카메라 오류. PLC는 상태 3일 때 칸 값을 사용합니다.
enum State { Wait, Learning, Settling, Ready, Fault };
State state=Wait;
Quad quad{};
bool configured=false, cameraOK=false, referenceOK=false, learning=false;
// indices[칸][측정점]은 사진에서 읽을 픽셀 위치입니다. 배열 번호 0은 현장의 1번 칸입니다.
uint32_t indices[Cells][N];
// 같은 칸의 같은 점을 비교합니다. background는 빈 판의 기준색, sample은 방금 찍은 색입니다.
uint16_t background[Cells][N], sample[Cells][N];
uint16_t sums[Cells][N][3];
Cell cells[Cells];
int scores[Cells], learned=0, stableFrames=0;
uint16_t sequence=0;
uint32_t startTime=0;
constexpr int LearnFrames=8;
bool locked=false;

// 모서리를 변경하거나 학습을 다시 시작할 때 지난 판정과 연속 프레임 수를 초기화합니다.
void resetCells() {
  for(int i=0;i<Cells;i++) { cells[i]=Cell(); scores[i]=0; }
  stableFrames=0;
}
// 웹 버튼 또는 PLC 명령으로 호출합니다. 사람이 아홉 칸을 비운 상태에서 실행해야 합니다.
// 카메라의 자동 노출과 색 보정을 켜서 현재 조명에 적응시킵니다.
void beginLearning() {
  if(!configured || !cameraOK) return;
  resetCells();
  referenceOK=false; learning=true; state=Learning; learned=0;
  memset(sums,0,sizeof sums);
  sensor_t *s=esp_camera_sensor_get();
  s->set_exposure_ctrl(s,1); s->set_gain_ctrl(s,1);
  s->set_whitebal(s,1); s->set_awb_gain(s,1);
  locked=false; startTime=millis();
}
// 빈 판 학습 1회분입니다. millis()는 부팅 후 지난 밀리초입니다.
// 1.8초 적응 후 보정값을 고정하고, 0.4초가 더 지나면 기준색을 수집합니다.
void learnFrame() {
  if(!locked) {
    if(millis()-startTime<1800) return;
    sensor_t *s=esp_camera_sensor_get();
    s->set_exposure_ctrl(s,0); s->set_gain_ctrl(s,0);
    s->set_whitebal(s,0); s->set_awb_gain(s,0);
    locked=true; startTime=millis(); return;
  }
  if(millis()-startTime<400) return;
  for(int k=0;k<Cells;k++) for(int j=0;j<N;j++) {
    uint16_t p=sample[k][j];
    // a는 이번 칸·측정점의 빨강/초록/파랑 누적합을 가리킵니다. 8장의 색을 더해 반올림한 평균을 저장합니다.
    uint16_t *a=sums[k][j];
    a[0]+=p>>11; a[1]+=(p>>5)&63; a[2]+=p&31;
    if(learned+1==LearnFrames)
      background[k][j]=((a[0]+4)/8<<11)|((a[1]+4)/8<<5)|((a[2]+4)/8);
  }
  Serial.printf("[LEARN] %d/%d\n",learned+1,LearnFrames);
  if(++learned==LearnFrames) { learning=false; referenceOK=true; state=Settling; }
}
// 사진이 들어왔는지, 크기와 색 형식이 예상값인지 검사해 잘못된 메모리 접근을 막습니다.
bool goodFrame(camera_fb_t *fb) {
  return fb && fb->buf && fb->format==PIXFORMAT_RGB565 && fb->width==W && fb->height==H && fb->len>=W*H*2;
}
// 사진 한 장에서 아홉 칸을 모두 검사합니다. fb는 카메라가 빌려준 사진 버퍼를 가리킵니다.
void measure() {
  if(!cameraOK){state=Fault;return;}
  camera_fb_t *fb=esp_camera_fb_get();
  if(!goodFrame(fb)) {
    if(fb) esp_camera_fb_return(fb);
    if(learning) {learning=false; referenceOK=false;}
    resetCells();state=Fault;return;
  }
  if(configured) for(int k=0;k<Cells;k++) for(int j=0;j<N;j++) {
    // RGB565의 한 픽셀은 2바이트입니다. 위치에 2를 곱해 색을 읽고 두 바이트를 16비트 값 하나로 합칩니다.
    const uint8_t *p=fb->buf+2*indices[k][j];
    sample[k][j]=(uint16_t(p[0])<<8)|p[1];
  }
  esp_camera_fb_return(fb);
  if(learning){learnFrame();return;}
  if(!referenceOK){state=Wait;return;}
  for(int k=0;k<Cells;k++){
    // 칸 k의 변화율을 구하고, 연속 3프레임 기준으로 그 칸의 유무를 갱신합니다.
    scores[k]=score(background[k],sample[k]);
    cells[k].update(scores[k]);
  }
  if(stableFrames<Stable) ++stableFrames;
  state=stableFrames>=Stable?Ready:Settling;
  ++sequence;
}

// PLC의 Modbus 요청을 성공적으로 처리할 때마다 1 증가합니다. 카메라 촬영 횟수와는 별도입니다.
uint32_t plcRequests=0;
Modbus::ResultCode countRequest(Modbus::FunctionCode, const Modbus::RequestData) {
  ++plcRequests;
  return Modbus::EX_SUCCESS;
}

// USB 시리얼 모니터에 상태가 바뀐 순간과 5초 주기로 출력합니다. cells 문자열은 왼쪽부터 1~9번입니다.
void reportProgress() {
  static const char *names[]={"WAIT_REFERENCE","LEARNING","SETTLING","READY","CAMERA_FAULT"};
  static int previousState=-1, previousMask=-1;
  static uint32_t lastReport=0;
  int mask=0;
  char bits[Cells+1]={};
  for(int k=0;k<Cells;++k) {
    bits[k]=cells[k].occupied ? '1' : '0';
    if(cells[k].occupied) mask |= 1<<k;
  }
// static 변수는 다음 호출에도 값을 유지합니다. 지난 출력과 같으면 5초가 될 때까지 출력을 건너뜁니다.
  if(previousState==state && previousMask==mask && millis()-lastReport<5000) return;
  previousState=state;
  previousMask=mask;
  lastReport=millis();
  Serial.printf("[%lu ms] state=%s cells=%s frame=%u plc_ok=%lu\n",
                (unsigned long)lastReport,names[state],state==Ready?bits:"---------",
                unsigned(sequence),(unsigned long)plcRequests);
}
// PLC가 FC03으로 읽을 때 실제 판정값을 반환합니다. 예: 40014는 5번 칸이므로 cells[4]를 읽습니다.
uint16_t expose(TRegister *reg,uint16_t) {
  int address=reg->address.address;
  if(address>=FirstCell && address<Status) return cells[address-FirstCell].occupied;
  return address==Status ? uint16_t(state) : sequence;
}
// 브라우저 요청을 처리합니다. /frame은 사진, /grid는 모서리 저장, /learn은 빈 판 학습, /status는 현재 결과입니다.
void routes(){
  web.on("/",[](){web.send_P(200,"text/html;charset=utf-8",PAGE);});
  web.on("/frame",[](){
    camera_fb_t *fb=cameraOK?esp_camera_fb_get():nullptr;
    uint8_t *jpg=nullptr; size_t len=0;
    bool ok=goodFrame(fb) && fmt2jpg(fb->buf,fb->len,W,H,PIXFORMAT_RGB565,85,&jpg,&len);
    if(fb) esp_camera_fb_return(fb);
    if(ok){web.sendHeader("Cache-Control","no-store");web.setContentLength(len);
      web.send(200,"image/jpeg","");web.client().write(jpg,len);}
    else web.send(503,"text/plain","camera unavailable");
    free(jpg);
  });
  web.on("/grid",HTTP_POST,[](){
// 브라우저가 보낸 4쌍의 x,y를 임시 저장한 뒤, 모두 유효한 경우에만 기존 설정을 교체합니다.
    Quad candidate{}; auto &p=candidate.p; char trailing;
    int count=sscanf(web.arg("p").c_str(),"%d,%d,%d,%d,%d,%d,%d,%d%c",&p[0].x,&p[0].y,&p[1].x,&p[1].y,&p[2].x,&p[2].y,&p[3].x,&p[3].y,&trailing);
    if(count!=8 || !valid(candidate)){web.send(400,"text/plain;charset=utf-8","모서리 좌표와 순서를 확인하세요.");return;}
    if(prefs.putBytes("quad",&candidate,sizeof candidate)!=sizeof candidate){web.send(500,"text/plain","save failed");return;}
    Serial.println("[GRID] corners saved");
    quad=candidate;map(quad,indices);configured=true;learning=referenceOK=false;
    resetCells();state=cameraOK?Wait:Fault;
    web.send(200,"text/plain;charset=utf-8","모서리를 저장했습니다. 빈 판 학습을 실행하세요.");
  });
  web.on("/learn",HTTP_POST,[](){
    if(!configured||!cameraOK){web.send(409,"text/plain;charset=utf-8","카메라와 모서리 설정을 확인하세요.");return;}
    beginLearning();web.send(200,"text/plain;charset=utf-8","학습을 시작했습니다.");
  });
  web.on("/status",[](){
    String s="{\"state\":"+String(int(state))+",\"frame\":"+String(sequence)+",\"points\":[";
    if(configured) for(int i=0;i<4;i++){if(i)s+=",";s+="["+String(quad.p[i].x)+","+String(quad.p[i].y)+"]";}
    s+="],\"cells\":["; for(int i=0;i<Cells;i++){if(i)s+=",";s+=String(int(cells[i].occupied));}
    s+="],\"scores\":[";for(int i=0;i<Cells;i++){if(i)s+=",";s+=String(scores[i]);}
    web.send(200,"application/json",s+"]}");
  });
  web.begin();
}
// 전원을 켜거나 리셋했을 때 한 번 실행됩니다. Serial은 PC용 USB 모니터, Serial2는 PLC용 RS-485 통신입니다.
void setup(){
  Serial.begin(115200);
  Serial.println("[BOOT] 9-section camera");
  Serial2.begin(38400,SERIAL_8N1,15,14);
  mb.begin(&Serial2,-1);mb.slave(SlaveId);
// PLC가 읽고 쓸 주소를 등록합니다. onGetHreg는 PLC가 읽기를 요청할 때 expose 함수를 호출하도록 연결합니다.
  mb.addHreg(Command,0,Sequence-Command+1);
  mb.onGetHreg(FirstCell,expose,Sequence-FirstCell+1);
  mb.onRequestSuccess(countRequest);
  Serial.printf("[RTU] 38400 8N1 ID=%u\n",unsigned(SlaveId));
  prefs.begin("tray9",false);
  configured=prefs.getBytesLength("quad")==sizeof quad &&
    prefs.getBytes("quad",&quad,sizeof quad)==sizeof quad && valid(quad);
  if(configured) map(quad,indices);
  cameraOK=startCamera();state=cameraOK?Wait:Fault;
  WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.begin(ssid,password);
  Serial.printf("[CAM] init=%s grid=%s\n",cameraOK?"OK":"FAIL",configured?"saved":"unset");
  Serial.printf("[WiFi] connecting SSID=%s\n",ssid);
  routes();
}
// 전원이 켜져 있는 동안 반복합니다. PLC 요청과 웹 요청을 처리하면서 약 100 ms 간격으로 촬영을 시도합니다.
void loop(){
  mb.task();
  if(mb.Hreg(Command)==1 && !learning) beginLearning();
  web.handleClient();
  // 같은 LAN에서 카메라를 이름으로 찾도록 mDNS를 등록합니다.
  // 연결이 끊기면 등록을 정리하고, 재연결되면 현재 IP로 다시 알립니다.
  static bool online=false, nameRegistered=false;
  static uint32_t lastAttempt=0;
  const bool connected=WiFi.isConnected();
  const bool justConnected=connected && !online;
  if(online && !connected){
    if(nameRegistered) MDNS.end();
    nameRegistered=false;
    Serial.println("[WiFi] disconnected");
  }
  online=connected;
  if(justConnected){Serial.print("[WiFi] IP=");Serial.println(WiFi.localIP());}
  if(connected && !nameRegistered && (justConnected || millis()-lastAttempt>=5000)){
    lastAttempt=millis();
    // begin에는 .local을 제외한 이름을 전달합니다. 웹 서버는 80번 포트를 사용합니다.
    nameRegistered=MDNS.begin("link-cam2");
    if(nameRegistered){
      MDNS.addService("http","tcp",80);
      Serial.println("[WEB] http://link-cam2.local/");
    } else Serial.println("[mDNS] registration failed; retry in 5 s");
  }
  static uint32_t last=0;
  if(millis()-last>=100){last=millis();measure();}
// PLC가 40009를 읽었을 때 학습 중이면 2, 그 외에는 0이 보이도록 갱신합니다.
  mb.Hreg(Command,learning?2:0);
  reportProgress();
  delay(1);
}
