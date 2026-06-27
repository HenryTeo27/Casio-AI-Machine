#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include "esp_camera.h"
#include "mbedtls/base64.h"
#include "casio_config.h"

#define OLED_SDA 1
#define OLED_SCL 2
#define OLED_RST 47
#define BTN_SCROLL_UP 14
#define BTN_SCROLL_DOWN 21
#define BTN_SCROLL_LEFT 40
#define BTN_SCROLL_RIGHT 41
#define BTN_CAMERA 38
#define BTN_ENTER 39
#define BTN_PAGE 42
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

#define SCREEN_W 128
#define SCREEN_H 32
#define MAX_PHOTOS_PER_QUESTION 3
#define MAX_LOCAL_DRAFTS 4
#define MAX_LIST_ITEMS 20
#define MAX_DETAIL_CACHE_ITEMS 3
#define BUTTON_DEBOUNCE_MS 22
#define OLED_CONTRAST 0
#define OLED_I2C_CLOCK_HZ 50000
#define OLED_REFRESH_MS 6000
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define HTTP_TIMEOUT_MS 60000
#define UPLOAD_TIMEOUT_MS 60000
#define NETWORK_RETRY_DELAY_MS 900
#define LIST_REFRESH_MS 12000
#define PROCESSING_POLL_MS 3500
#define DETAIL_RETRY_MS 2500
#define THERMAL_CHECK_MS 3000
#define THERMAL_WARN_C 70.0f
#define THERMAL_SHUTDOWN_C 78.0f
#define THERMAL_RECOVER_C 62.0f
#define CAM_WARMUP_FRAMES 3
#define CAM_WARMUP_DELAY_MS 180
#define CAM_SETTLE_AFTER_INIT_MS 500
#define CAM_CAPTURE_JPEG_QUALITY 10
#define CAM_WARMUP_JPEG_QUALITY 12
#define CAM_FALLBACK_JPEG_QUALITY 12
#define CAM_CAPTURE_RETRIES 3

const char* const V2_SESSION_START_PATH = "/api/casio-ai/v2/session/start";
const char* const V2_SESSION_QUESTIONS_PATH = "/api/casio-ai/v2/session/questions";
const char* const V2_QUESTIONS_PATH = "/api/casio-ai/v2/questions";

U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);
Preferences prefs;
SemaphoreHandle_t stateMutex = nullptr;

struct Button {
  uint8_t pin = 0; bool lastStable = HIGH; bool lastRead = HIGH; unsigned long lastChange = 0; unsigned long lastRepeat = 0;
  void begin(uint8_t p) { pin = p; pinMode(pin, INPUT_PULLUP); lastStable = digitalRead(pin); lastRead = lastStable; lastChange = millis(); }
  bool fell() { bool r = digitalRead(pin); if (r != lastRead) { lastChange = millis(); lastRead = r; } if ((millis() - lastChange) >= BUTTON_DEBOUNCE_MS && r != lastStable) { lastStable = r; if (lastStable == LOW) lastRepeat = millis(); return lastStable == LOW; } return false; }
  bool repeat(unsigned long firstDelayMs = 360, unsigned long repeatMs = 140) { if (lastStable != LOW) return false; unsigned long now = millis(); if ((now - lastChange) < firstDelayMs) return false; if ((now - lastRepeat) < repeatMs) return false; lastRepeat = now; return true; }
};

struct DisplayBlock { String kind = "text"; int width = SCREEN_W; int height = SCREEN_H; std::vector<uint8_t> packedXbm; int xOffset = 0; int yOffset = 0; };
struct PendingPhoto { uint8_t* data = nullptr; size_t len = 0; uint16_t width = 0; uint16_t height = 0; bool uploaded = false; String photoId = ""; };
struct DraftQuestion { int localNo = 0; int displayOrder = 0; String localListId = ""; String createToken = ""; String serverId = ""; String status = "LOCAL_DRAFT"; bool submitted = false; bool solveStarted = false; unsigned long nextActionMs = 0; std::vector<PendingPhoto> photos; };
struct QuestionItem { String id = ""; int displayOrder = 0; String status = ""; String summary = ""; String answer = ""; String error = ""; int photoCount = 0; int blockCount = 0; bool detailRequested = false; bool detailLoaded = false; bool blockFetchComplete = false; bool viewedLastBlock = false; unsigned long nextPollMs = 0; unsigned long lastAccessMs = 0; std::vector<DisplayBlock> blocks; int blockIndex = 0; std::vector<String> fallbackLines; };
enum ScreenMode { MODE_BOOT, MODE_LIST, MODE_CAPTURE, MODE_VIEW, MODE_ERROR, MODE_THERMAL };

Button btnUp, btnDown, btnLeft, btnRight, btnCamera, btnEnter, btnPage;
ScreenMode screenMode = MODE_BOOT;
String lastError = "", sessionId = "", lastOledLine1 = "", lastOledLine2 = "", lastBitmapKey = "";
bool sessionReady = false, screenDirty = true, answerBlankMode = false, cameraSessionOn = false, captureBusy = false;
bool thermalWarning = false, thermalShutdown = false;
float lastChipTempC = -1000.0f;
unsigned long nextListRefreshMs = 0, nextThermalCheckMs = 0, nextOledRefreshMs = 0;
int selectedIndex = 0, activeDraftIndex = -1, nextLocalDraftNo = 1, focusSubmittedLocalNo = 0, backgroundPrefetchCursor = 0;
std::vector<QuestionItem> questionList;
std::vector<DraftQuestion> drafts;

void lockState(){ if(stateMutex) xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY); }
void unlockState(){ if(stateMutex) xSemaphoreGiveRecursive(stateMutex); }
void markScreenDirty(){ screenDirty = true; }
void setScreenMode(ScreenMode m){ if(screenMode != m){ screenMode = m; lastOledLine1=""; lastOledLine2=""; lastBitmapKey=""; markScreenDirty(); } }
String safeShort(const String& v,int n){ return (int)v.length() <= n ? v : v.substring(0,n); }
String rawPreview(const String& raw,size_t maxLen=160){ String p=raw.substring(0,min((int)raw.length(),(int)maxLen)); p.replace("\r","\\r"); p.replace("\n","\\n"); return p; }
std::vector<String> wrapFallbackLines(const String& text,int maxChars=20){ std::vector<String> out; String s=text; s.replace("\r",""); s.replace("\n"," "); s.trim(); while(s.length()){ int n=min(maxChars,(int)s.length()); out.push_back(s.substring(0,n)); s=s.substring(n); } if(out.empty()) out.push_back(""); return out; }
void showError(const String& e){ lastError=safeShort(e,21); Serial.printf("[ERR] %s\n",e.c_str()); setScreenMode(MODE_ERROR); }
void freePendingPhoto(PendingPhoto& p){ if(p.data){ free(p.data); p.data=nullptr; } p.len=0; p.width=0; p.height=0; }
bool isWorkingStatus(const String& s){ return s=="DRAFT"||s=="CREATING"||s=="QUEUED"||s=="UPLOADING"||s=="UPLOADED"||s=="PROCESSING"||s=="ANSWER_READY"||s=="RENDERING"; }
int findQuestionIndexById(const String& id){ for(int i=0;i<(int)questionList.size();i++) if(questionList[i].id==id) return i; return -1; }
void removeQuestionItemById(const String& id){ int idx=findQuestionIndexById(id); if(idx>=0) questionList.erase(questionList.begin()+idx); selectedIndex=min(selectedIndex,max(0,(int)questionList.size()-1)); }
void upsertQuestionItem(const QuestionItem& in,bool preserveDetail=true){
  int idx=findQuestionIndexById(in.id);
  if(idx>=0){
    QuestionItem old=questionList[idx];
    bool becameReady=old.status!="SUCCEEDED"&&in.status=="SUCCEEDED";
    questionList[idx].displayOrder=in.displayOrder?in.displayOrder:old.displayOrder;
    questionList[idx].status=in.status;
    if(in.summary.length()) questionList[idx].summary=in.summary;
    questionList[idx].photoCount=in.photoCount;
    questionList[idx].error=in.error;
    questionList[idx].nextPollMs=in.nextPollMs;
    questionList[idx].blockCount=in.blockCount?in.blockCount:old.blockCount;
    if(!preserveDetail || !old.detailLoaded){
      int keepBlockIndex=old.blockIndex;
      questionList[idx].answer=in.answer;
      questionList[idx].blocks=in.blocks;
      questionList[idx].blockCount=in.blockCount;
      questionList[idx].detailLoaded=in.detailLoaded;
      questionList[idx].blockFetchComplete=in.blockFetchComplete;
      questionList[idx].fallbackLines=in.fallbackLines;
      questionList[idx].blockIndex=min(max(0,keepBlockIndex),max(0,(int)questionList[idx].blocks.size()-1));
    } else {
      questionList[idx].blockFetchComplete=old.blockFetchComplete;
    }
    questionList[idx].viewedLastBlock=old.viewedLastBlock;
    if(becameReady&&!questionList[idx].detailLoaded){
      questionList[idx].detailRequested=true;
      questionList[idx].nextPollMs=0;
      Serial.printf("[V2] auto-prefetch ready q=%s blocks=%d\n",in.id.c_str(),questionList[idx].blockCount);
    }
    return;
  }
  questionList.push_back(in);
  std::sort(questionList.begin(),questionList.end(),[](const QuestionItem& a,const QuestionItem& b){ return a.displayOrder<b.displayOrder; });
  while((int)questionList.size()>MAX_LIST_ITEMS) questionList.pop_back();
  selectedIndex=min(selectedIndex,max(0,(int)questionList.size()-1));
}
void evictDetailCacheIfNeeded(){ int cached=0; for(auto& q:questionList) if(!q.blocks.empty()) cached++; while(cached>MAX_DETAIL_CACHE_ITEMS){ int ev=-1; unsigned long oldest=ULONG_MAX; for(int i=0;i<(int)questionList.size();i++){ if(i==selectedIndex||questionList[i].blocks.empty()) continue; if(questionList[i].lastAccessMs<oldest){ oldest=questionList[i].lastAccessMs; ev=i; } } if(ev<0) break; questionList[ev].blocks.clear(); questionList[ev].detailLoaded=false; questionList[ev].blockFetchComplete=false; questionList[ev].viewedLastBlock=false; cached--; } }
void releaseDisplayCacheForCapture(){ int freed=0; lockState(); for(auto& q:questionList){ if(q.blocks.empty()&&q.fallbackLines.empty()) continue; q.blocks.clear(); q.fallbackLines.clear(); q.detailLoaded=false; q.blockFetchComplete=false; q.viewedLastBlock=false; freed++; } if(freed>0){ lastBitmapKey=""; Serial.printf("[MEM] released %d answer cache(s) for camera heap=%u psram=%u\n",freed,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram()); } unlockState(); }
void releaseViewedAnswerCacheOnLeave(){ if(screenMode!=MODE_VIEW||selectedIndex<0||selectedIndex>=(int)questionList.size()) return; QuestionItem& q=questionList[selectedIndex]; if(!q.viewedLastBlock||q.blocks.empty()) return; int count=q.blocks.size(); q.blocks.clear(); q.fallbackLines.clear(); q.detailLoaded=false; q.blockFetchComplete=false; q.viewedLastBlock=false; q.blockIndex=0; lastBitmapKey=""; Serial.printf("[MEM] released viewed answer q=%s blocks=%d\n",q.id.c_str(),count); }
DraftQuestion* currentDraft(){ if(activeDraftIndex<0||activeDraftIndex>=(int)drafts.size()) return nullptr; return &drafts[activeDraftIndex]; }
bool isEditableDraft(const DraftQuestion& d){ return !d.submitted && (int)d.photos.size()<MAX_PHOTOS_PER_QUESTION; }
int maxKnownDisplayOrder(){ int m=0; for(auto& q:questionList) m=max(m,q.displayOrder); for(auto& d:drafts) m=max(m,d.displayOrder); return m; }
int findEditableDraftIndex(){ if(activeDraftIndex>=0&&activeDraftIndex<(int)drafts.size()&&isEditableDraft(drafts[activeDraftIndex])) return activeDraftIndex; for(int i=0;i<(int)drafts.size();i++) if(isEditableDraft(drafts[i])) return i; return -1; }
int findDraftIndexByLocalNo(int localNo){ for(int i=0;i<(int)drafts.size();i++) if(drafts[i].localNo==localNo) return i; return -1; }
bool createEditableDraft(){ if((int)drafts.size()>=MAX_LOCAL_DRAFTS) return false; DraftQuestion d; d.localNo=nextLocalDraftNo++; d.displayOrder=maxKnownDisplayOrder()+1; d.localListId="draft:"+String(d.localNo); d.createToken=String(DEVICE_ID)+":"+sessionId+":local:"+String(d.localNo)+":"+String(millis()); d.photos.reserve(MAX_PHOTOS_PER_QUESTION); drafts.push_back(std::move(d)); activeDraftIndex=drafts.size()-1; Serial.printf("[DRAFT] new local=%d display=%d queued=%u/%d\n",drafts[activeDraftIndex].localNo,drafts[activeDraftIndex].displayOrder,(unsigned)drafts.size(),MAX_LOCAL_DRAFTS); return true; }
bool ensureDraft(){ int idx=findEditableDraftIndex(); if(idx>=0){ activeDraftIndex=idx; return true; } return createEditableDraft(); }
void moveToNextDraft(){ DraftQuestion* d=currentDraft(); if(d&&isEditableDraft(*d)&&d->photos.empty()) return; createEditableDraft(); }
bool hasAnyDraftPhotos(){ for(auto& d:drafts) if(!d.submitted&&!d.photos.empty()) return true; return false; }
bool hasSubmittedDrafts(){ for(auto& d:drafts) if(d.submitted&&!d.solveStarted) return true; return false; }
void updateDraftPlaceholderStatus(const DraftQuestion& d,const String& status,const String& summary=""){ if(!d.localListId.length()) return; lockState(); int qi=findQuestionIndexById(d.localListId); if(qi>=0){ questionList[qi].status=status; if(summary.length()) questionList[qi].summary=summary; questionList[qi].photoCount=d.photos.size(); markScreenDirty(); } unlockState(); }
void submitDraftsWithPhotos(){ bool any=false; focusSubmittedLocalNo=0; lockState(); for(auto& d:drafts){ if(!d.photos.empty()&&!d.submitted){ d.submitted=true; d.status="QUEUED"; d.nextActionMs=millis(); if(!focusSubmittedLocalNo)focusSubmittedLocalNo=d.localNo; QuestionItem item; item.id=d.localListId; item.displayOrder=d.displayOrder; item.status="QUEUED"; item.photoCount=d.photos.size(); item.summary="Queued"; upsertQuestionItem(item,true); selectedIndex=findQuestionIndexById(item.id); Serial.printf("[DRAFT] submit local=%d photos=%u placeholder=%s\n",d.localNo,(unsigned)d.photos.size(),d.localListId.c_str()); any=true; } } activeDraftIndex=-1; if(any){ nextListRefreshMs=millis()+2000; setScreenMode(MODE_LIST); } unlockState(); }

void drawTwoLines(const String& a,const String& b){ if(a!=lastOledLine1||b!=lastOledLine2) Serial.printf("[OLED] %s || %s\n",a.c_str(),b.c_str()); lastOledLine1=a; lastOledLine2=b; lastBitmapKey=""; u8g2.clearBuffer(); u8g2.setDrawColor(1); u8g2.setFontMode(1); u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(0,12,a.c_str()); u8g2.drawStr(0,28,b.c_str()); u8g2.sendBuffer(); }
void clearOledFrame(){ u8g2.clearBuffer(); u8g2.sendBuffer(); delay(8); }
int maxXForBlock(const DisplayBlock& b){ return max(0,b.width-SCREEN_W); }
int maxYForBlock(const DisplayBlock& b){ return max(0,b.height-SCREEN_H); }
void renderBitmapBlock(const DisplayBlock& b){ String key=String((uintptr_t)b.packedXbm.data())+":"+String(b.width)+"x"+String(b.height)+":"+String(b.xOffset)+":"+String(b.yOffset); if(key==lastBitmapKey) return; bool firstBitmap=(lastOledLine1!="__bitmap__"); lastBitmapKey=key; lastOledLine1="__bitmap__"; lastOledLine2=""; int xo=min(max(0,b.xOffset),maxXForBlock(b)); int yo=min(max(0,b.yOffset),maxYForBlock(b)); u8g2.clearBuffer(); u8g2.setDrawColor(1); u8g2.setFontMode(1); if(!b.packedXbm.empty()) u8g2.drawXBMP(-xo,-yo,b.width,b.height,b.packedXbm.data()); u8g2.sendBuffer(); if(firstBitmap){ delay(8); u8g2.sendBuffer(); } }
String statusLabel(const QuestionItem& q){ if(q.status=="SUCCEEDED")return"ready"; if(q.status=="FAILED")return"ERR"; if(q.status=="RENDERING"||q.status=="ANSWER_READY")return"render"; if(q.status=="PROCESSING")return"thinking"; if(q.status=="UPLOADING"||q.status=="UPLOADED")return"upload"; if(q.status=="QUEUED")return"queued"; if(q.status=="CREATING")return"create"; if(q.status=="DRAFT")return"DRAFT"; return safeShort(q.status,8); }

bool decodeBase64ToBytes(const String& enc,std::vector<uint8_t>& out){ size_t req=0; int rc=mbedtls_base64_decode(nullptr,0,&req,(const unsigned char*)enc.c_str(),enc.length()); if(rc!=MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL&&rc!=0) return false; if(req==0) return false; out.assign(req,0); size_t wr=0; rc=mbedtls_base64_decode(out.data(),out.size(),&wr,(const unsigned char*)enc.c_str(),enc.length()); if(rc!=0) return false; if(wr!=out.size()) out.resize(wr); return true; }
int parseHttpStatusCode(const String& line){ int a=line.indexOf(' '); if(a<0)return-1; int b=line.indexOf(' ',a+1); return ((b>a)?line.substring(a+1,b):line.substring(a+1)).toInt(); }
bool parseRawHttpResponse(const String& raw,const char* tag,int& code,String& body){ code=-1; body=""; if(!raw.length()){ Serial.printf("[%s] empty raw\n",tag); return false; } int h=raw.indexOf("\r\n\r\n"), bs=h>=0?h+4:-1; if(h<0){ h=raw.indexOf("\n\n"); bs=h>=0?h+2:-1; } if(h>=0){ int se=raw.indexOf('\n'); if(se<0||se>h)se=h; String sl=raw.substring(0,se); sl.trim(); code=parseHttpStatusCode(sl); body=raw.substring(bs); Serial.printf("[%s] status=%d body=%u raw=%u\n",tag,code,(unsigned)body.length(),(unsigned)raw.length()); return code>0; } if(raw.startsWith("HTTP/")){ Serial.printf("[%s] malformed %s\n",tag,rawPreview(raw).c_str()); return false; } code=200; body=raw; return true; }
bool connectWiFi(){ WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS); unsigned long st=millis(); while(WiFi.status()!=WL_CONNECTED&&(millis()-st)<WIFI_CONNECT_TIMEOUT_MS) delay(250); if(WiFi.status()==WL_CONNECTED){ Serial.printf("[WIFI] %s\n",WiFi.localIP().toString().c_str()); return true; } return false; }
bool ensureWiFiConnected(){ if(thermalWarning||thermalShutdown) return false; return WiFi.status()==WL_CONNECTED||connectWiFi(); }
bool httpsJsonRequest(const String& method,const String& path,const String& payload,int timeoutMs,int& code,String& body){ code=-1; body=""; if(!ensureWiFiConnected()) return false; WiFiClientSecure c; c.setInsecure(); c.setTimeout(timeoutMs/1000); if(!c.connect(SERVER_HOST,443)){ Serial.printf("[HTTP] connect failed %s\n",path.c_str()); return false; } String req; req.reserve(520+payload.length()); req+=method+" "+path+" HTTP/1.0\r\nHost: "+String(SERVER_HOST)+"\r\nConnection: close\r\nX-Device-Id: "+String(DEVICE_ID)+"\r\nX-Device-Api-Key: "+String(DEVICE_API_KEY); if(method=="POST") req+="\r\nContent-Type: application/json\r\nContent-Length: "+String(payload.length()); req+="\r\n\r\n"; c.print(req); if(method=="POST"&&payload.length()) c.print(payload); String raw; raw.reserve(4096); unsigned long last=millis(); while(true){ while(c.available()){ raw+=(char)c.read(); last=millis(); } if(!c.connected()&&!c.available()) break; if((millis()-last)>(unsigned long)timeoutMs){ c.stop(); return false; } delay(10); } c.stop(); return parseRawHttpResponse(raw,"HTTP",code,body); }
bool httpsBinaryUpload(const String& path,const uint8_t* data,size_t len,const String& token,int& code,String& body){ code=-1; body=""; if(!ensureWiFiConnected()) return false; WiFiClientSecure c; c.setInsecure(); c.setTimeout(UPLOAD_TIMEOUT_MS/1000); if(!c.connect(SERVER_HOST,443)) return false; String req="POST "+path+" HTTP/1.0\r\nHost: "+String(SERVER_HOST)+"\r\nConnection: close\r\nContent-Type: image/jpeg\r\nX-Device-Id: "+String(DEVICE_ID)+"\r\nX-Device-Api-Key: "+String(DEVICE_API_KEY); if(token.length()) req+="\r\nX-Upload-Token: "+token; req+="\r\nContent-Length: "+String(len)+"\r\n\r\n"; c.print(req); size_t sent=0; unsigned long last=millis(); while(sent<len){ size_t want=min((size_t)1024,len-sent); size_t w=c.write(data+sent,want); if(!w){ if((millis()-last)>UPLOAD_TIMEOUT_MS){ c.stop(); return false; } delay(2); continue; } sent+=w; last=millis(); delay(1); } String raw; raw.reserve(2048); last=millis(); while(true){ while(c.available()){ raw+=(char)c.read(); last=millis(); } if(!c.connected()&&!c.available()) break; if((millis()-last)>UPLOAD_TIMEOUT_MS){ c.stop(); return false; } delay(10); } c.stop(); return parseRawHttpResponse(raw,"UP",code,body); }

bool isSupportedBitmapHeight(int h){ return h==16||h==SCREEN_H||h==64; }
bool parseDisplayBlockItem(JsonVariant item,int page,DisplayBlock& out){ String type=(const char*)(item["type"]|""); String fmt=(const char*)(item["format"]|""); int w=item["width"]|0,h=item["height"]|0; if(type!="bitmap"||fmt!="1bit_xbm"||w<=0||!isSupportedBitmapHeight(h)){ Serial.printf("[OLED] reject block %d type=%s fmt=%s w=%d h=%d\n",page,type.c_str(),fmt.c_str(),w,h); return false; } String b64=(const char*)(item["data"]|""); std::vector<uint8_t> dec; if(!decodeBase64ToBytes(b64,dec)){ Serial.printf("[OLED] b64 fail block %d len=%u\n",page,(unsigned)b64.length()); return false; } int exp=((w+7)/8)*h; if((int)dec.size()!=exp){ Serial.printf("[OLED] bad block %d bytes=%u exp=%d w=%d h=%d\n",page,(unsigned)dec.size(),exp,w,h); return false; } out.kind=(const char*)(item["kind"]|"text"); out.width=w; out.height=h; out.xOffset=0; out.yOffset=0; out.packedXbm=std::move(dec); return true; }
bool parseBlocksArray(JsonVariant arr,std::vector<DisplayBlock>& out){ out.clear(); if(!arr.is<JsonArray>()) return false; int p=0; for(JsonVariant it:arr.as<JsonArray>()){ DisplayBlock b; if(parseDisplayBlockItem(it,++p,b)) out.push_back(std::move(b)); } return !out.empty(); }

bool apiStartSession(){ JsonDocument d; d["device_id"]=DEVICE_ID; if(sessionId.length()) d["session_id"]=sessionId; d["firmware_version"]="main-v2"; String payload; serializeJson(d,payload); int code; String body; if(!httpsJsonRequest("POST",V2_SESSION_START_PATH,payload,HTTP_TIMEOUT_MS,code,body)||code<200||code>=300) return false; JsonDocument r; if(deserializeJson(r,body)||!(r["ok"]|false)) return false; sessionId=(const char*)(r["session_id"]|""); if(!sessionId.length()) return false; prefs.putString("session_id",sessionId); sessionReady=true; Serial.printf("[V2] session=%s\n",sessionId.c_str()); return true; }
bool apiFetchSessionList(){ if(!sessionId.length()) return false; String path=String(V2_SESSION_QUESTIONS_PATH)+"?session_id="+sessionId+"&limit="+String(MAX_LIST_ITEMS); int code; String body; if(!httpsJsonRequest("GET",path,"",HTTP_TIMEOUT_MS,code,body)||code<200||code>=300) return false; JsonDocument doc; if(deserializeJson(doc,body)||!(doc["ok"]|false)) return false; std::vector<QuestionItem> fresh; for(JsonVariant it:doc["items"].as<JsonArray>()){ QuestionItem q; q.id=(const char*)(it["question_id"]|""); if(!q.id.length()) continue; q.displayOrder=it["display_order"]|0; q.status=(const char*)(it["status"]|""); q.photoCount=it["photo_count"]|0; q.summary=(const char*)(it["summary"]|""); q.nextPollMs=isWorkingStatus(q.status)?millis()+PROCESSING_POLL_MS:0; fresh.push_back(std::move(q)); } lockState(); for(auto& q:fresh) upsertQuestionItem(q,true); while((int)questionList.size()>MAX_LIST_ITEMS) questionList.pop_back(); selectedIndex=min(selectedIndex,max(0,(int)questionList.size()-1)); markScreenDirty(); unlockState(); Serial.printf("[V2] list fetched=%u\n",(unsigned)fresh.size()); return true; }
bool apiCreateQuestion(DraftQuestion& draft){ JsonDocument d; d["device_id"]=DEVICE_ID; d["session_id"]=sessionId; d["mode"]="oled-v2"; d["client_request_id"]=draft.createToken; String payload; serializeJson(d,payload); int code; String body; if(!httpsJsonRequest("POST",V2_QUESTIONS_PATH,payload,HTTP_TIMEOUT_MS,code,body)||code<200||code>=300) return false; JsonDocument r; if(deserializeJson(r,body)||!(r["ok"]|false)) return false; draft.serverId=(const char*)(r["question_id"]|""); draft.status="UPLOADING"; int order=r["display_order"]|draft.displayOrder; if(!draft.serverId.length()) return false; draft.displayOrder=order; QuestionItem item; item.id=draft.serverId; item.displayOrder=order; item.status="UPLOADING"; item.photoCount=draft.photos.size(); item.summary=(r["deduped"]|false)?"Uploading retry":"Uploading"; lockState(); removeQuestionItemById(draft.localListId); upsertQuestionItem(item,true); if(!focusSubmittedLocalNo||focusSubmittedLocalNo==draft.localNo){ selectedIndex=findQuestionIndexById(item.id); focusSubmittedLocalNo=0; } markScreenDirty(); unlockState(); Serial.printf("[DRAFT] server local=%d q=%s display=%d dedup=%d\n",draft.localNo,draft.serverId.c_str(),draft.displayOrder,(int)(r["deduped"]|false)); return true; }
bool apiUploadPhoto(DraftQuestion& draft,int idx){ if(idx<0||idx>=(int)draft.photos.size()) return false; PendingPhoto& p=draft.photos[idx]; if(p.uploaded) return true; size_t beforeFree=p.len; String token=String(DEVICE_ID)+":"+draft.serverId+":"+String(idx)+":"+String(p.len); String path=String(V2_QUESTIONS_PATH)+"/"+draft.serverId+"/photos?index="+String(idx); int code; String body; if(!httpsBinaryUpload(path,p.data,p.len,token,code,body)||code<200||code>=300) return false; JsonDocument r; if(deserializeJson(r,body)||!(r["ok"]|false)) return false; p.photoId=(const char*)(r["photo_id"]|""); p.uploaded=true; freePendingPhoto(p); Serial.printf("[UP] q=%s photo=%d uploaded and freed %u bytes\n",draft.serverId.c_str(),idx+1,(unsigned)beforeFree); lockState(); int qi=findQuestionIndexById(draft.serverId); if(qi>=0){ questionList[qi].status="UPLOADING"; questionList[qi].summary="Uploading "+String(idx+1)+"/"+String(draft.photos.size()); questionList[qi].photoCount=r["photo_count"]|(idx+1); } markScreenDirty(); unlockState(); return true; }
bool apiStartSolve(DraftQuestion& draft){ String path=String(V2_QUESTIONS_PATH)+"/"+draft.serverId+"/solve"; JsonDocument d; d["device_id"]=DEVICE_ID; d["mode"]="oled-v2"; String payload; serializeJson(d,payload); int code; String body; if(!httpsJsonRequest("POST",path,payload,HTTP_TIMEOUT_MS,code,body)||(code!=202&&(code<200||code>=300))) return false; JsonDocument r; if(deserializeJson(r,body)) return false; String st=(const char*)(r["status"]|"PROCESSING"); draft.status=st; draft.solveStarted=true; lockState(); int qi=findQuestionIndexById(draft.serverId); if(qi>=0){ questionList[qi].status=st; questionList[qi].summary=st; questionList[qi].nextPollMs=millis()+PROCESSING_POLL_MS; } markScreenDirty(); unlockState(); return true; }
bool apiFetchBlock(const String& qid,int idx,DisplayBlock& out){ String path=String(V2_QUESTIONS_PATH)+"/"+qid+"/blocks/"+String(idx); int code; String body; if(!httpsJsonRequest("GET",path,"",HTTP_TIMEOUT_MS,code,body)||code<200||code>=300){ Serial.printf("[V2] block fetch fail q=%s idx=%d code=%d\n",qid.c_str(),idx,code); return false; } JsonDocument d; if(deserializeJson(d,body)||!(d["ok"]|false)){ Serial.printf("[V2] block json fail q=%s idx=%d body=%s\n",qid.c_str(),idx,rawPreview(body).c_str()); return false; } return parseDisplayBlockItem(d["block"],idx+1,out); }
bool apiFetchQuestionDetail(const String& qid,bool wantBlocks){
  String path=String(V2_QUESTIONS_PATH)+"/"+qid;
  int code;
  String body;
  if(!httpsJsonRequest("GET",path,"",HTTP_TIMEOUT_MS,code,body)) return false;
  JsonDocument d;
  if(deserializeJson(d,body)){
    Serial.printf("[V2] detail json fail %s\n",rawPreview(body).c_str());
    return false;
  }

  String st=(const char*)(d["status"]|"");
  QuestionItem u;
  u.id=qid;
  u.displayOrder=d["display_order"]|0;
  u.status=st;
  u.photoCount=d["photo_count"]|0;
  u.summary=(const char*)(d["summary"]|st.c_str());
  u.error=(const char*)(d["error"]|"");
  u.answer=(const char*)(d["answer"]|"");
  u.blockCount=d["display_block_count"]|0;
  u.nextPollMs=isWorkingStatus(st)?millis()+PROCESSING_POLL_MS:0;

  if(st=="SUCCEEDED"&&wantBlocks){
    std::vector<DisplayBlock> existingBlocks;
    int existingIndex=0;
    bool existingComplete=false;
    lockState();
    int currentIndex=findQuestionIndexById(qid);
    if(currentIndex>=0){
      existingBlocks=questionList[currentIndex].blocks;
      existingIndex=questionList[currentIndex].blockIndex;
      existingComplete=questionList[currentIndex].blockFetchComplete;
    }
    unlockState();

    bool inlineOk=parseBlocksArray(d["display_blocks"],u.blocks);
    if(inlineOk){
      u.blockFetchComplete=(u.blockCount<=0)||((int)u.blocks.size()>=u.blockCount);
      Serial.printf("[V2] inline blocks q=%s got=%u count=%d complete=%d\n",qid.c_str(),(unsigned)u.blocks.size(),u.blockCount,(int)u.blockFetchComplete);
    } else if(u.blockCount>0){
      u.blocks=std::move(existingBlocks);
      u.blockIndex=min(max(0,existingIndex),max(0,(int)u.blocks.size()-1));
      u.blockFetchComplete=existingComplete&&((int)u.blocks.size()>=u.blockCount);

      int start=(int)u.blocks.size();
      int maxFetch=1;
      for(int i=start;i<u.blockCount&&maxFetch>0;i++,maxFetch--){
        DisplayBlock b;
        if(!apiFetchBlock(qid,i,b)){
          Serial.printf("[V2] partial blocks q=%s got=%u/%d next=%d\n",qid.c_str(),(unsigned)u.blocks.size(),u.blockCount,i);
          break;
        }
        u.blocks.push_back(std::move(b));
      }
      u.blockFetchComplete=(int)u.blocks.size()>=u.blockCount;
      Serial.printf("[V2] block cache q=%s got=%u/%d complete=%d\n",qid.c_str(),(unsigned)u.blocks.size(),u.blockCount,(int)u.blockFetchComplete);
    }
    u.detailLoaded=!u.blocks.empty();
    if(!u.detailLoaded){
      Serial.printf("[V2] bitmap missing q=%s count=%d, wait for render repair\n",qid.c_str(),u.blockCount);
      u.status="RENDERING";
      u.summary="render";
      u.answer="";
      u.blockCount=0;
      u.nextPollMs=millis()+PROCESSING_POLL_MS;
    } else {
      u.fallbackLines=wrapFallbackLines(u.answer.length()?u.answer:u.summary);
    }
  }

  lockState();
  int i=findQuestionIndexById(qid);
  bool preserve=!(st=="SUCCEEDED"&&wantBlocks&&u.detailLoaded);
  if(i>=0){
    if(!u.displayOrder) u.displayOrder=questionList[i].displayOrder;
    bool wasReady=questionList[i].status=="SUCCEEDED";
    bool req=questionList[i].detailRequested;
    upsertQuestionItem(u,preserve);
    i=findQuestionIndexById(qid);
    if(i>=0){
      questionList[i].detailRequested=req||wantBlocks||(!wasReady&&u.status=="SUCCEEDED");
      questionList[i].lastAccessMs=millis();
    }
  } else upsertQuestionItem(u,preserve);
  evictDetailCacheIfNeeded();
  markScreenDirty();
  unlockState();
  return code>=200&&code<300;
}

void cleanupFinishedDrafts();
bool initCamera(){ camera_config_t c={}; bool ps=psramFound(); c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0; c.pin_d0=CAM_PIN_D0; c.pin_d1=CAM_PIN_D1; c.pin_d2=CAM_PIN_D2; c.pin_d3=CAM_PIN_D3; c.pin_d4=CAM_PIN_D4; c.pin_d5=CAM_PIN_D5; c.pin_d6=CAM_PIN_D6; c.pin_d7=CAM_PIN_D7; c.pin_xclk=CAM_PIN_XCLK; c.pin_pclk=CAM_PIN_PCLK; c.pin_vsync=CAM_PIN_VSYNC; c.pin_href=CAM_PIN_HREF; c.pin_sccb_sda=CAM_PIN_SIOD; c.pin_sccb_scl=CAM_PIN_SIOC; c.pin_pwdn=CAM_PIN_PWDN; c.pin_reset=CAM_PIN_RESET; c.xclk_freq_hz=20000000; c.pixel_format=PIXFORMAT_JPEG; c.frame_size=ps?FRAMESIZE_SXGA:FRAMESIZE_XGA; c.jpeg_quality=CAM_CAPTURE_JPEG_QUALITY; c.fb_count=1; c.fb_location=ps?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM; c.grab_mode=CAMERA_GRAB_LATEST; if(esp_camera_init(&c)!=ESP_OK) return false; sensor_t* s=esp_camera_sensor_get(); if(s){ s->set_framesize(s,ps?FRAMESIZE_SXGA:FRAMESIZE_XGA); s->set_quality(s,CAM_CAPTURE_JPEG_QUALITY); s->set_whitebal(s,1); s->set_awb_gain(s,1); s->set_wb_mode(s,0); s->set_brightness(s,2); s->set_contrast(s,2); s->set_saturation(s,0); s->set_exposure_ctrl(s,1); s->set_gain_ctrl(s,1); s->set_aec2(s,1); s->set_ae_level(s,2); s->set_gainceiling(s,GAINCEILING_2X); if(s->set_sharpness)s->set_sharpness(s,-2); if(s->set_denoise)s->set_denoise(s,2); if(s->set_bpc)s->set_bpc(s,1); if(s->set_wpc)s->set_wpc(s,1); if(s->set_lenc)s->set_lenc(s,1); if(s->set_raw_gma)s->set_raw_gma(s,1); if(s->set_vflip)s->set_vflip(s,1); if(s->set_hmirror)s->set_hmirror(s,0); } cameraSessionOn=true; Serial.printf("[CAM] init ok frame=%s q=%d\n",ps?"SXGA":"XGA",CAM_CAPTURE_JPEG_QUALITY); return true; }
void sensorStandby(){ sensor_t* s=esp_camera_sensor_get(); if(s&&s->set_reg){ s->set_reg(s,0x3008,0xFF,0x42); delay(80); } }
void quietCameraPins(){ pinMode(CAM_PIN_XCLK,OUTPUT); digitalWrite(CAM_PIN_XCLK,LOW); const int pins[]={CAM_PIN_SIOD,CAM_PIN_SIOC,CAM_PIN_D0,CAM_PIN_D1,CAM_PIN_D2,CAM_PIN_D3,CAM_PIN_D4,CAM_PIN_D5,CAM_PIN_D6,CAM_PIN_D7,CAM_PIN_VSYNC,CAM_PIN_HREF,CAM_PIN_PCLK}; for(int p:pins) if(p>=0) pinMode(p,INPUT); }
void stopCameraForCooling(){ if(cameraSessionOn){ sensorStandby(); esp_camera_deinit(); cameraSessionOn=false; } quietCameraPins(); }
float readChipTempC(){ return temperatureRead(); }
void enterThermalScreen(){ answerBlankMode=false; setScreenMode(MODE_THERMAL); markScreenDirty(); }
void enterThermalShutdown(){ thermalShutdown=true; thermalWarning=true; stopCameraForCooling(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF); setCpuFrequencyMhz(80); Serial.printf("[THERMAL] shutdown temp=%.1fC\n",lastChipTempC); enterThermalScreen(); }
void checkThermalGuard(bool force=false){ unsigned long now=millis(); if(!force&&now<nextThermalCheckMs)return; nextThermalCheckMs=now+THERMAL_CHECK_MS; float t=readChipTempC(); if(t<-100.0f||t>150.0f)return; lastChipTempC=t; if(t>=THERMAL_SHUTDOWN_C){ if(!thermalShutdown) enterThermalShutdown(); else enterThermalScreen(); return; } if(thermalShutdown){ enterThermalScreen(); return; } if(t>=THERMAL_WARN_C){ if(!thermalWarning){ Serial.printf("[THERMAL] warning temp=%.1fC\n",t); stopCameraForCooling(); WiFi.setSleep(true); } thermalWarning=true; enterThermalScreen(); return; } if(thermalWarning&&t<=THERMAL_RECOVER_C){ thermalWarning=false; Serial.printf("[THERMAL] recovered temp=%.1fC\n",t); if(screenMode==MODE_THERMAL) setScreenMode(MODE_LIST); markScreenDirty(); } }
camera_fb_t* captureFrame(sensor_t* s){ for(int i=0;i<CAM_CAPTURE_RETRIES;i++){ camera_fb_t* fb=esp_camera_fb_get(); if(fb)return fb; delay(120); } if(s&&s->set_framesize){ s->set_framesize(s,FRAMESIZE_XGA); if(s->set_quality)s->set_quality(s,CAM_FALLBACK_JPEG_QUALITY); delay(700); for(int i=0;i<2;i++){ camera_fb_t* w=esp_camera_fb_get(); if(w)esp_camera_fb_return(w); delay(180); } for(int i=0;i<CAM_CAPTURE_RETRIES;i++){ camera_fb_t* fb=esp_camera_fb_get(); if(fb)return fb; delay(120); } } return nullptr; }
bool captureOnePhotoToDraft(){ if(thermalWarning||thermalShutdown){ enterThermalScreen(); return false; } if(captureBusy)return false; captureBusy=true; cleanupFinishedDrafts(); releaseDisplayCacheForCapture(); int localNo=0; lockState(); if(!ensureDraft()){ unlockState(); captureBusy=false; drawTwoLines("Queue full","Wait upload"); return false; } DraftQuestion* d=currentDraft(); if(!d||!isEditableDraft(*d)){ unlockState(); captureBusy=false; return false; } localNo=d->localNo; unlockState(); drawTwoLines("Capturing...","Please wait"); uint32_t t0=millis(); if(!initCamera()){ captureBusy=false; stopCameraForCooling(); showError("Camera init fail"); return false; } delay(CAM_SETTLE_AFTER_INIT_MS); sensor_t* s=esp_camera_sensor_get(); if(s&&s->set_quality)s->set_quality(s,CAM_WARMUP_JPEG_QUALITY); for(int i=0;i<CAM_WARMUP_FRAMES;i++){ camera_fb_t* w=esp_camera_fb_get(); if(w)esp_camera_fb_return(w); delay(CAM_WARMUP_DELAY_MS); } if(s&&s->set_quality){ s->set_quality(s,CAM_CAPTURE_JPEG_QUALITY); delay(500); } camera_fb_t* fb=captureFrame(s); if(!fb){ captureBusy=false; stopCameraForCooling(); showError("Capture failed"); return false; } PendingPhoto p; p.data=(uint8_t*)ps_malloc(fb->len); if(!p.data)p.data=(uint8_t*)malloc(fb->len); if(!p.data){ Serial.printf("[MEM] photo alloc failed len=%u heap=%u psram=%u, retry after cache release\n",(unsigned)fb->len,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram()); releaseDisplayCacheForCapture(); p.data=(uint8_t*)ps_malloc(fb->len); if(!p.data)p.data=(uint8_t*)malloc(fb->len); } if(!p.data){ esp_camera_fb_return(fb); captureBusy=false; stopCameraForCooling(); showError("No PSRAM"); return false; } memcpy(p.data,fb->buf,fb->len); p.len=fb->len; p.width=fb->width; p.height=fb->height; esp_camera_fb_return(fb); stopCameraForCooling(); int photoNo=0; lockState(); int di=findDraftIndexByLocalNo(localNo); if(di<0||!isEditableDraft(drafts[di])){ unlockState(); freePendingPhoto(p); captureBusy=false; showError("Draft gone"); return false; } drafts[di].photos.push_back(p); p.data=nullptr; photoNo=drafts[di].photos.size(); activeDraftIndex=di; unlockState(); Serial.printf("[CAM] draft=%d #%d bytes=%u w=%u h=%u total=%lums temp=%.1fC heap=%u psram=%u\n",localNo,photoNo,(unsigned)p.len,p.width,p.height,(unsigned long)(millis()-t0),lastChipTempC,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram()); captureBusy=false; markScreenDirty(); return true; }

bool processOneDraft(){ int di=-1; lockState(); unsigned long now=millis(); for(int i=0;i<(int)drafts.size();i++){ if(drafts[i].submitted&&!drafts[i].solveStarted&&drafts[i].nextActionMs<=now){ di=i; break; } } unlockState(); if(di<0) return false; DraftQuestion& d=drafts[di]; if(!d.serverId.length()){ d.status="CREATING"; updateDraftPlaceholderStatus(d,"CREATING","Creating"); if(!apiCreateQuestion(d)){ Serial.printf("[DRAFT] create retry local=%d\n",d.localNo); d.nextActionMs=millis()+NETWORK_RETRY_DELAY_MS; return true; } } for(int i=0;i<(int)d.photos.size();i++){ if(!d.photos[i].uploaded){ d.status="UPLOADING"; updateDraftPlaceholderStatus(d,"UPLOADING","Uploading "+String(i+1)+"/"+String(d.photos.size())); if(!apiUploadPhoto(d,i)){ Serial.printf("[UP] retry q=%s photo=%d\n",d.serverId.c_str(),i+1); d.nextActionMs=millis()+NETWORK_RETRY_DELAY_MS; return true; } } } if(!apiStartSolve(d)){ Serial.printf("[SOLVE] retry q=%s\n",d.serverId.c_str()); d.nextActionMs=millis()+NETWORK_RETRY_DELAY_MS; return true; } d.nextActionMs=millis()+PROCESSING_POLL_MS; return true; }
bool pickQuestionBlocksForPoll(int i,unsigned long now,String& qid,bool& want,bool priority=false){
  if(i<0||i>=(int)questionList.size()) return false;
  bool selected=i==selectedIndex&&screenMode==MODE_VIEW;
  bool wantsDetail=selected||questionList[i].detailRequested;
  bool needsBlocks=questionList[i].status=="SUCCEEDED"&&wantsDetail&&(!questionList[i].detailLoaded||!questionList[i].blockFetchComplete);
  if(!needsBlocks) return false;
  bool ignoreDelay=priority&&!questionList[i].detailLoaded;
  if(!ignoreDelay&&questionList[i].nextPollMs>now) return false;
  qid=questionList[i].id;
  want=true;
  questionList[i].detailRequested=true;
  questionList[i].nextPollMs=now+DETAIL_RETRY_MS;
  return true;
}

bool pickQuestionStatusForPoll(int i,unsigned long now,String& qid,bool& want){
  if(i<0||i>=(int)questionList.size()) return false;
  if(questionList[i].id.startsWith("draft:")) return false;
  if(!isWorkingStatus(questionList[i].status)) return false;
  if(questionList[i].nextPollMs>now) return false;
  bool selected=i==selectedIndex&&screenMode==MODE_VIEW;
  want=selected||questionList[i].detailRequested;
  qid=questionList[i].id;
  questionList[i].nextPollMs=now+PROCESSING_POLL_MS;
  return true;
}

bool processOneQuestionPoll(){
  String qid="";
  bool want=false;
  lockState();
  unsigned long now=millis();
  if(screenMode==MODE_VIEW&&selectedIndex>=0&&selectedIndex<(int)questionList.size()){
    pickQuestionBlocksForPoll(selectedIndex,now,qid,want,true);
    if(!qid.length()) pickQuestionStatusForPoll(selectedIndex,now,qid,want);
  }
  if(!qid.length()){
    for(int i=0;i<(int)questionList.size();i++){
      if(i==selectedIndex&&screenMode==MODE_VIEW) continue;
      if(pickQuestionStatusForPoll(i,now,qid,want)) break;
    }
  }
  if(!qid.length()){
    int n=(int)questionList.size();
    for(int step=0;step<n;step++){
      int i=(backgroundPrefetchCursor+step)%n;
      if(i==selectedIndex&&screenMode==MODE_VIEW) continue;
      if(pickQuestionBlocksForPoll(i,now,qid,want,false)){
        backgroundPrefetchCursor=(i+1)%max(1,n);
        break;
      }
    }
  }
  unlockState();
  if(!qid.length()) return false;
  apiFetchQuestionDetail(qid,want);
  return true;
}
void cleanupFinishedDrafts(){ if(captureBusy) return; lockState(); for(int i=drafts.size()-1;i>=0;i--){ if(drafts[i].submitted&&drafts[i].solveStarted){ bool freed=true; for(auto& p:drafts[i].photos) if(p.data) freed=false; if(freed){ Serial.printf("[DRAFT] cleanup local=%d q=%s\n",drafts[i].localNo,drafts[i].serverId.c_str()); drafts.erase(drafts.begin()+i); if(activeDraftIndex==i) activeDraftIndex=-1; else if(activeDraftIndex>i) activeDraftIndex--; } } } if(drafts.empty()) activeDraftIndex=-1; unlockState(); }
void networkTask(void*){ Serial.println("[NET] worker started"); while(true){ if(thermalWarning||thermalShutdown){ delay(1000); continue; } if(!sessionReady){ if(apiStartSession()){ apiFetchSessionList(); nextListRefreshMs=millis()+LIST_REFRESH_MS; lockState(); if(!thermalWarning&&!thermalShutdown)setScreenMode(MODE_LIST); markScreenDirty(); unlockState(); } else { delay(1000); continue; } } if(processOneDraft()){ cleanupFinishedDrafts(); delay(50); continue; } if(processOneQuestionPoll()){ delay(50); continue; } if(millis()>=nextListRefreshMs){ apiFetchSessionList(); nextListRefreshMs=millis()+LIST_REFRESH_MS; } delay(120); } }

void renderList(){ if(!sessionReady){ drawTwoLines("OpenAI Calculator","Syncing session"); return; } if(questionList.empty()){ drawTwoLines("> New question","CAM: photo"); return; } selectedIndex=min(max(0,selectedIndex),(int)questionList.size()-1); int top=(selectedIndex/2)*2; int i1=top,i2=min((int)questionList.size()-1,top+1); String l1=String(i1==selectedIndex?">":" ")+"Q"+String(questionList[i1].displayOrder)+" "+statusLabel(questionList[i1]); String l2="ENTER=open"; if(i2!=i1) l2=String(i2==selectedIndex?">":" ")+"Q"+String(questionList[i2].displayOrder)+" "+statusLabel(questionList[i2]); if(l1!=lastOledLine1||l2!=lastOledLine2){ Serial.printf("[OLED] %s || %s\n",l1.c_str(),l2.c_str()); lastOledLine1=l1; lastOledLine2=l2; lastBitmapKey=""; } u8g2.clearBuffer(); u8g2.setDrawColor(1); u8g2.setFontMode(1); u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(0,12,l1.c_str()); u8g2.drawStr(0,28,l2.c_str()); u8g2.sendBuffer(); }
void renderCapture(){ if(!ensureDraft()){ drawTwoLines("Queue full","Wait upload"); return; } DraftQuestion* d=currentDraft(); int c=d?(int)d->photos.size():0; String l1="Draft "+String(d?d->localNo:0)+" ["+String(c)+"/"+String(MAX_PHOTOS_PER_QUESTION)+"]"; String l2=c>0?"CAM more ENT send":"CAM shot ENT back"; drawTwoLines(l1,l2); }
void renderView(){ if(selectedIndex<0||selectedIndex>=(int)questionList.size()){ drawTwoLines("No answer yet","CAM: capture"); return; } QuestionItem& q=questionList[selectedIndex]; q.lastAccessMs=millis(); if(q.status!="SUCCEEDED"){ drawTwoLines("Q"+String(q.displayOrder), q.error.length()?safeShort(q.error,21):statusLabel(q)); return; } if(answerBlankMode){ if(lastOledLine1!="__blank_style__"){ lastOledLine1="__blank_style__"; lastOledLine2=""; lastBitmapKey=""; Serial.println("[OLED] blank style mode"); clearOledFrame(); } return; } if(!q.detailLoaded||q.blocks.empty()){ q.detailRequested=true; q.nextPollMs=0; drawTwoLines("Q"+String(q.displayOrder),"Loading answer..."); return; } q.blockIndex=min(max(0,q.blockIndex),(int)q.blocks.size()-1); if(q.blockIndex==(int)q.blocks.size()-1) q.viewedLastBlock=true; renderBitmapBlock(q.blocks[q.blockIndex]); }
void renderThermal(){ String t=lastChipTempC>-100.0f?String(lastChipTempC,1)+"C":"checking"; drawTwoLines(thermalShutdown?"TEMP CRITICAL":"TEMP HIGH",thermalShutdown?("Power off "+t):("Cooling "+t)); }
void renderScreen(){ switch(screenMode){ case MODE_BOOT: drawTwoLines("OpenAI Calculator","Connecting WiFi"); break; case MODE_LIST: renderList(); break; case MODE_CAPTURE: renderCapture(); break; case MODE_VIEW: renderView(); break; case MODE_ERROR: drawTwoLines("ERROR",lastError); break; case MODE_THERMAL: renderThermal(); break; } }

void actionUp(){ answerBlankMode=false; if(screenMode==MODE_CAPTURE){ if(activeDraftIndex>0) activeDraftIndex--; markScreenDirty(); return; } if(screenMode==MODE_LIST){ if(selectedIndex>0) selectedIndex--; markScreenDirty(); return; } if(screenMode==MODE_VIEW&&selectedIndex>=0&&selectedIndex<(int)questionList.size()){ QuestionItem& q=questionList[selectedIndex]; if(!q.blocks.empty()){ DisplayBlock& b=q.blocks[q.blockIndex]; if(b.yOffset>0)b.yOffset=max(0,b.yOffset-SCREEN_H); else if(q.blockIndex>0){ q.blockIndex--; q.blocks[q.blockIndex].yOffset=maxYForBlock(q.blocks[q.blockIndex]); } markScreenDirty(); } } }
void actionDown(){ answerBlankMode=false; if(screenMode==MODE_CAPTURE){ moveToNextDraft(); markScreenDirty(); return; } if(screenMode==MODE_LIST){ if(selectedIndex<(int)questionList.size()-1) selectedIndex++; markScreenDirty(); return; } if(screenMode==MODE_VIEW&&selectedIndex>=0&&selectedIndex<(int)questionList.size()){ QuestionItem& q=questionList[selectedIndex]; if(!q.blocks.empty()){ DisplayBlock& b=q.blocks[q.blockIndex]; int my=maxYForBlock(b); if(b.yOffset<my)b.yOffset=min(my,b.yOffset+SCREEN_H); else if(q.blockIndex<(int)q.blocks.size()-1){ q.blockIndex++; q.blocks[q.blockIndex].yOffset=0; } markScreenDirty(); } } }
void actionLeft(){ answerBlankMode=false; if(screenMode==MODE_VIEW&&selectedIndex>=0&&selectedIndex<(int)questionList.size()){ QuestionItem& q=questionList[selectedIndex]; if(!q.blocks.empty()){ DisplayBlock& b=q.blocks[q.blockIndex]; if(b.xOffset>0)b.xOffset=max(0,b.xOffset-64); else if(q.blockIndex>0){ q.blockIndex--; q.blocks[q.blockIndex].xOffset=maxXForBlock(q.blocks[q.blockIndex]); } markScreenDirty(); } } }
void actionRight(){ answerBlankMode=false; if(screenMode==MODE_VIEW&&selectedIndex>=0&&selectedIndex<(int)questionList.size()){ QuestionItem& q=questionList[selectedIndex]; if(!q.blocks.empty()){ DisplayBlock& b=q.blocks[q.blockIndex]; int mx=maxXForBlock(b); if(b.xOffset<mx)b.xOffset=min(mx,b.xOffset+64); else if(q.blockIndex<(int)q.blocks.size()-1){ q.blockIndex++; q.blocks[q.blockIndex].xOffset=0; } markScreenDirty(); } } }
void actionCamera(){ answerBlankMode=false; if(thermalWarning||thermalShutdown){ enterThermalScreen(); return; } if(screenMode!=MODE_CAPTURE){ lockState(); releaseViewedAnswerCacheOnLeave(); bool ok=ensureDraft(); if(ok) setScreenMode(MODE_CAPTURE); unlockState(); if(!ok) drawTwoLines("Queue full","Wait upload"); return; } captureOnePhotoToDraft(); }
void actionEnter(){ if(screenMode==MODE_CAPTURE){ answerBlankMode=false; if(hasAnyDraftPhotos()) submitDraftsWithPhotos(); else setScreenMode(MODE_LIST); return; } if(screenMode==MODE_LIST){ answerBlankMode=false; if(!questionList.empty()){ questionList[selectedIndex].detailRequested=true; questionList[selectedIndex].nextPollMs=0; setScreenMode(MODE_VIEW); } else if(hasSubmittedDrafts()){ markScreenDirty(); } else { ensureDraft(); setScreenMode(MODE_CAPTURE); } return; } if(screenMode==MODE_VIEW&&selectedIndex>=0&&selectedIndex<(int)questionList.size()&&questionList[selectedIndex].status=="SUCCEEDED"){ answerBlankMode=!answerBlankMode; markScreenDirty(); } }
void actionPage(){ answerBlankMode=false; if(screenMode==MODE_LIST){ if(!questionList.empty()){ questionList[selectedIndex].detailRequested=true; questionList[selectedIndex].nextPollMs=0; setScreenMode(MODE_VIEW); } else setScreenMode(MODE_CAPTURE); } else { releaseViewedAnswerCacheOnLeave(); setScreenMode(MODE_LIST); } }

void setup(){ Serial.begin(115200); delay(700); Serial.println("\n=== Casio AI Machine Main V2 ==="); stateMutex=xSemaphoreCreateRecursiveMutex(); drafts.reserve(MAX_LOCAL_DRAFTS); questionList.reserve(MAX_LIST_ITEMS); prefs.begin("casio-ai",false); sessionId=prefs.getString("session_id",""); btnUp.begin(BTN_SCROLL_UP); btnDown.begin(BTN_SCROLL_DOWN); btnLeft.begin(BTN_SCROLL_LEFT); btnRight.begin(BTN_SCROLL_RIGHT); btnCamera.begin(BTN_CAMERA); btnEnter.begin(BTN_ENTER); btnPage.begin(BTN_PAGE); Wire.begin(OLED_SDA,OLED_SCL); Wire.setClock(OLED_I2C_CLOCK_HZ); u8g2.setI2CAddress(0x3C*2); u8g2.begin(); u8g2.setBusClock(OLED_I2C_CLOCK_HZ); u8g2.setContrast(OLED_CONTRAST); u8g2.enableUTF8Print(); quietCameraPins(); screenMode=MODE_BOOT; renderScreen(); nextOledRefreshMs=millis()+OLED_REFRESH_MS; checkThermalGuard(true); BaseType_t ok=xTaskCreatePinnedToCore(networkTask,"netTask",24000,NULL,1,NULL,1); if(ok!=pdPASS) showError("Net task fail"); }
void loop(){ checkThermalGuard(); bool locked=thermalWarning||thermalShutdown; if(btnUp.fell()||btnUp.repeat()){ Serial.println("[BTN] UP"); if(locked)enterThermalScreen(); else { lockState(); actionUp(); unlockState(); } } if(btnDown.fell()||btnDown.repeat()){ Serial.println("[BTN] DOWN"); if(locked)enterThermalScreen(); else { lockState(); actionDown(); unlockState(); } } if(btnLeft.fell()||btnLeft.repeat()){ Serial.println("[BTN] LEFT"); if(locked)enterThermalScreen(); else { lockState(); actionLeft(); unlockState(); } } if(btnRight.fell()||btnRight.repeat()){ Serial.println("[BTN] RIGHT"); if(locked)enterThermalScreen(); else { lockState(); actionRight(); unlockState(); } } if(btnCamera.fell()){ Serial.println("[BTN] CAMERA"); if(locked)enterThermalScreen(); else actionCamera(); } if(btnEnter.fell()){ Serial.println("[BTN] ENTER"); if(locked)enterThermalScreen(); else { lockState(); actionEnter(); unlockState(); } } if(btnPage.fell()){ Serial.println("[BTN] PAGE"); if(locked)enterThermalScreen(); else { lockState(); actionPage(); unlockState(); } } if(millis()>=nextOledRefreshMs){ if(screenMode!=MODE_LIST){ lastOledLine1=""; lastOledLine2=""; lastBitmapKey=""; screenDirty=true; } nextOledRefreshMs=millis()+OLED_REFRESH_MS; } if(screenDirty){ lockState(); renderScreen(); screenDirty=false; unlockState(); } delay(20); }
