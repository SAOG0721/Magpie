"""Test the production manual migration writer/loader and JsonHelper without an app build."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
source = (repo / 'src/Magpie/ScalingModesService.cpp').read_text(encoding='utf-8-sig')
writer = source[source.index('static void WriteScalingMode('):source.index('\nvoid ScalingModesService::Export(')]
loader = source[source.index('static bool LoadScalingMode('):source.index('\nstruct V065NormalizationStats')]
normalizer = source[source.index('struct V065NormalizationStats'):source.index('\nbool ScalingModesService::Import(')]
importer = source[source.index('bool ScalingModesService::Import('):source.rfind('\n}')]
helper = (repo / 'src/Magpie/JsonHelper.cpp').read_text(encoding='utf-8-sig')
helper = helper[helper.index('bool JsonHelper::ReadBool('):helper.rfind('\n}')]
settings = (repo / 'src/Magpie/AppSettings.cpp').read_text(encoding='utf-8-sig')
rules_start = settings.index('const ConfigRecovery::ParameterRules rules = []')
rules_end = settings.index('\n\t\t};', rules_start) + len('\n\t\t};')
recovery_rules = '\nConfigRecovery::ParameterRules ProductionRecoveryRules() {\n' + settings[rules_start:rules_end] + '\nreturn rules;\n}\n'
prefix = r'''
#include <windows.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include "HdrManualMigration.h"
#include "ConfigRecovery.h"
#include "OpticalFlowDefaults.h"
#include "ScalingModeNames.h"
#include "DLSSNRDetailParameters.h"
#include "FrameGenerationParameters.h"
#include "OKLabConfig.h"
#include "OKLabParameters.h"
#include "RTXVideoParameters.h"
#include "XeSSFGParameters.h"
#include "DlssOpticalFlowParameters.h"
#include "JsonHelper.h"
#include <cassert>
#include <iostream>
#include <map>
#include <fstream>
#include <sstream>
#include <span>
#include <tuple>
#include <variant>
#include <type_traits>
namespace fmt { template<class... Args> std::string format(const char*,Args...) { return {}; } }
struct StrHelper {
 static std::wstring UTF8ToUTF16(std::string_view text) {
  std::wstring output(MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0),L'\0');
  MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),output.data(),int(output.size())); return output;
 }
 static std::string UTF16ToUTF8(std::wstring_view text) {
  std::string output(WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr),'\0');
  WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),output.data(),int(output.size()),nullptr,nullptr); return output;
 }
};
namespace Magpie {
template<class T> struct EffectConstant { T defaultValue,minValue,maxValue,step; };
struct EffectParameterDesc {
 std::string name;
 struct Choice { int value; };
 std::variant<EffectConstant<float>,EffectConstant<int>> constant;
 std::vector<Choice> choices;
};
struct EffectsService {
 struct Info { std::vector<EffectParameterDesc> params; };
 static EffectsService& Get() { static EffectsService value;return value; }
 const Info* GetEffect(std::wstring_view name) const {
  static const Info nr{{
   {"processingMode",EffectConstant<int>{0,0,1,1},{{0},{1}}},
   {"sparseUpdateInterval",EffectConstant<int>{2,2,4,1},{{2},{3},{4}}}}};
  return name==L"DLSSNR\\DLSSNR_AI_Filter"?&nr:nullptr;
 }
};
enum class ScalingType : uint32_t { Normal,Fit,Absolute,Fill };
struct ParameterMap : std::map<std::wstring,float,std::less<>> {
 using Base=std::map<std::wstring,float,std::less<>>; using Base::Base; using Base::erase;
 void reserve(size_t) {} size_t erase(std::wstring_view key) { return Base::erase(std::wstring(key)); }
 size_t erase(const wchar_t* key) { return Base::erase(std::wstring(key)); }
};
struct EffectItem {
 std::wstring name;
 ParameterMap parameters;
 ScalingType scalingType=ScalingType::Normal;
 std::pair<float,float> scale{1.f,1.f};
 bool isRecoveryInvalid=false;
 std::string recoveryOriginal;
 std::string oklabLut;
 bool HasScale() const { return scalingType!=ScalingType::Normal || scale!=std::pair<float,float>{1.f,1.f}; }
};
struct ScalingMode {
 std::wstring name;
 std::vector<EffectItem> effects;
 std::optional<bool> legacyHdrCaptureIntent;
 std::string hdrMigrationBackup;
 bool hdrMigrationInvalid=false;
 std::string hdrMigrationRecovery;
};
struct AppSettings {
 std::vector<ScalingMode> modes; unsigned marks=0;
 static AppSettings& Get() { static AppSettings value; return value; }
 auto& ScalingModes() { return modes; } void MarkConfigMigrationNeeded() { ++marks; } void SaveAsync() {}
};
struct Logger { static Logger& Get() { static Logger value; return value; } void Info(const auto&) {} };
enum class EffectAddedWay { Import };
struct ScalingModesService {
 struct Event { void Invoke(EffectAddedWay) {} } ScalingModeAdded;
 bool Import(const rapidjson::GenericObject<true,rapidjson::Value>&,bool,uint32_t*) noexcept;
};
'''
tests = r'''
std::string Save(const ScalingMode& value) {
 rapidjson::StringBuffer buffer; rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
 WriteScalingMode(writer,value); return std::string(buffer.GetString(),buffer.GetSize());
}
ScalingMode Load(std::string_view text, bool loadingSettings=true) {
 rapidjson::Document json; json.Parse(text.data(),text.size()); assert(!json.HasParseError());
 ScalingMode result; assert(LoadScalingMode(std::as_const(json).GetObj(),result,loadingSettings)); return result;
}
std::string Encode(const rapidjson::Value& value) {
 rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b); value.Accept(w);
 return std::string(b.GetString(),b.GetSize());
}
rapidjson::Document Trial(unsigned input,unsigned output,const char* effects) {
 const std::string json=std::string(R"({"name":"试用 α","hdr":{"version":1,"managed":true,"input":)")+
 std::to_string(input)+R"(,"output":)"+std::to_string(output)+R"(,"nativeLegacy":false,
 "whiteNits":203,"peakNits":1000,"exposure":1,"shoulder":1,"nativeWhiteNits":1500,
 "legacyWhiteNits":203,"legacyExposure":1,"legacyPeakNits":1000,
 "rtxPeakNits":1701,"rtxContrast":133,"rtxSaturation":101,"rtxMiddleGray":51},"effects":)"+effects+"}";
 rapidjson::Document result; result.Parse(json.c_str()); assert(!result.HasParseError()); return result;
}
void VerifyMigration(const rapidjson::Document& original, const ScalingMode& mode) {
 assert(!mode.hdrMigrationInvalid && mode.hdrMigrationBackup==Encode(original));
 const auto saved=Save(mode); rapidjson::Document json; json.Parse(saved.c_str());
 assert(!json.HasMember("hdr") && json.HasMember("hdrManualMigration"));
 const auto& old=original["effects"]; const auto& now=json["effects"]; assert(old.Size()==now.Size());
 for (rapidjson::SizeType i=0; i<now.Size(); ++i) {
  assert(!now[i].HasMember("hdrManagedRole"));
  assert(Encode(old[i]["name"])==Encode(now[i]["name"]));
  for (const char* key:{"parameters","scalingType","scale"}) {
   if (std::string_view(key)=="parameters"&&std::string_view(old[i]["name"].GetString())=="DLSSNR\\DLSSNR_AI_Filter") {
    ParameterMap expected;
    if(old[i].HasMember(key))for(const auto& parameter:old[i][key].GetObj())
     expected[StrHelper::UTF8ToUTF16(parameter.name.GetString())]=parameter.value.GetFloat();
    NormalizeDLSSNRDetailParameters(expected);
    assert(now[i].HasMember(key)&&now[i][key].MemberCount()==expected.size());
    for(const auto& [name,value]:expected) {
     const auto utf8=StrHelper::UTF16ToUTF8(name);
     assert(now[i][key].HasMember(utf8.c_str())&&now[i][key][utf8.c_str()].GetFloat()==value);
    }
    continue;
   }
   assert(old[i].HasMember(key)==now[i].HasMember(key));
   if (!old[i].HasMember(key)) continue;
   if (old[i][key].IsObject()) {
    assert(old[i][key].MemberCount()==now[i][key].MemberCount());
    for (const auto& param:old[i][key].GetObj())
     assert(now[i][key].HasMember(param.name) && param.value.GetFloat()==now[i][key][param.name].GetFloat());
   } else assert(old[i][key].GetUint()==now[i][key].GetUint());
  }
 }
 const auto again=Load(saved); assert(Save(again)==saved && again.hdrMigrationBackup==mode.hdrMigrationBackup);
 assert(again.legacyHdrCaptureIntent==mode.legacyHdrCaptureIntent);
 // Import follows the same migration contract instead of re-planning.
 const auto imported=Load(Encode(original),false); assert(Save(imported)==saved);
}
void VerifyRejected(const rapidjson::Document& original) {
 const auto recovery=ConfigRecovery::Prepare(std::string("{\"version\":4,\"scalingModes\":[")+Encode(original)+"]}","","Recovered",[](auto,auto) { return std::optional<ConfigRecovery::ParameterRule>{{80,400,203,false,{}}}; });
 assert(Encode(recovery.document["scalingModes"][0])==Encode(original));
 const auto mode=Load(Encode(recovery.document["scalingModes"][0])); assert(mode.hdrMigrationInvalid && mode.hdrMigrationRecovery==Encode(original));
 rapidjson::Document saved; saved.Parse(Save(mode).c_str()); assert(Encode(saved)==Encode(original));
 const auto again=Load(Save(mode)); assert(again.hdrMigrationInvalid && Save(again)==Save(mode));
 ScalingMode imported; assert(!LoadScalingMode(std::as_const(original).GetObj(),imported,false));
}
ScalingMode FullImport(const rapidjson::Document& mode,unsigned expectedMarks) {
 rapidjson::Document root; root.SetObject();
 rapidjson::Value modes(rapidjson::kArrayType); rapidjson::Value item; item.CopyFrom(mode,root.GetAllocator());
 modes.PushBack(item,root.GetAllocator()); root.AddMember("scalingModes",modes,root.GetAllocator());
 auto& app=AppSettings::Get(); app.modes.clear(); app.marks=0; ScalingModesService service;
 assert(service.Import(std::as_const(root).GetObj(),true,nullptr) && app.modes.size()==1);
 assert(app.marks==expectedMarks); return app.modes.front();
}
void VerifyDlssnrModeMigration() {
 const int legacyModes[]{0,1,0,0,0,1,0},versionedModes[]{0,1,0,1};
 for (int version : {0,1,2}) for (int mode=0;mode<(version?4:7);++mode)
  for (int interval : {-1,0,2,3,4,5}) {
   const std::string parameters=std::string(R"({"processingMode":)")+std::to_string(mode)+
    (version?R"(,"__dlssnrModeVersion":)"+std::to_string(version):"")+
    (interval>=0?R"(,"sparseUpdateInterval":)"+std::to_string(interval):"")+
    R"(,"recoveryPolicy":2,"bufferedDelayFrames":1,"asyncResidualComposition":0,
      "modelPreset":2,"resolutionControl":1,"inputResolutionPercent":75,"residualTransfer":1,
      "opticalFlowMethod":2,"amdOpticalFlowMode":1,"nvidiaOpticalFlowQuality":5,
      "multiPass":3,"style":2,"pass2_intensity":1.5,"pass3_style":1,
      "antiFlicker":4,"antiFlickerTimeMs":95,"residualChromaProtection":0.75})";
   const std::string text=R"({"name":"DLSSNR schema roundtrip","effects":[{"name":"DLSSNR\\DLSSNR_AI_Filter","parameters":)"+parameters+"}]}";
   rapidjson::Document raw;raw.Parse(text.c_str());assert(!raw.HasParseError());
   // AppSettings validates generic parameters before the versioned loader.
   // Its actual rule callback must preserve old mode 5 and the internal marker.
   const auto recovery=ConfigRecovery::Prepare(std::string(R"({"version":4,"scalingModes":[)")+text+"]}","","Recovered",ProductionRecoveryRules());
   const auto& recovered=recovery.document["scalingModes"][0];
   assert(recovered["effects"][0]["parameters"]["processingMode"].GetInt()==mode);
   if(version)assert(recovered["effects"][0]["parameters"]["__dlssnrModeVersion"].GetInt()==version);
   rapidjson::Document prepared;prepared.CopyFrom(recovered,prepared.GetAllocator());
   const auto imported=FullImport(prepared,1);
   const auto& values=imported.effects.at(0).parameters;
   const int expectedMode=version?versionedModes[mode]:legacyModes[mode];
   const int expectedInterval=!version&&mode==1?2:(interval>=2&&interval<=4?interval:2);
   assert(values.at(L"processingMode")==expectedMode&&values.at(L"sparseUpdateInterval")==expectedInterval);
   assert(values.at(L"__dlssnrModeVersion")==DLSSNR_MODE_VERSION);
   for (const auto key:{L"recoveryPolicy",L"bufferedDelayFrames",L"asyncResidualComposition"})assert(!values.contains(key));
   assert(values.at(L"modelPreset")==2&&values.at(L"inputResolutionPercent")==75&&values.at(L"multiPass")==3);
   assert(values.at(L"style")==2&&values.at(L"pass2_intensity")==1.5f&&values.at(L"pass3_style")==1);
   assert(values.at(L"opticalFlowMethod")==2&&values.at(L"nvidiaOpticalFlowQuality")==5&&values.at(L"amdOpticalFlowMode")==1);
   assert(values.at(L"antiFlicker")==4&&values.at(L"antiFlickerTimeMs")==95&&values.at(L"residualChromaProtection")==.75f);
   const auto saved=Save(imported);const auto loaded=Load(saved);
   assert(Save(loaded)==saved&&loaded.effects[0].parameters==values);
   rapidjson::Document reopened;reopened.Parse(saved.c_str());
   assert(Save(FullImport(reopened,0))==saved);
   auto copied=loaded;copied.name=L"DLSSNR schema copy";
   assert(!NormalizeDLSSNRDetailParameters(copied.effects[0].parameters));
   assert(Load(Save(copied)).effects[0].parameters==values);
   // Supported edits remain stable; retired raw values cannot re-enter on save.
   for (int edited : {0,1,2,3}) {
    copied.effects[0].parameters[L"processingMode"]=float(edited);
    const auto normalized=Load(Save(copied));
    assert(normalized.effects[0].parameters.at(L"processingMode")==versionedModes[edited]);
    assert(Save(Load(Save(normalized)))==Save(normalized));
   }
  }
 std::cout<<"PASS: DLSSNR legacy/v1/two-mode migration, hidden legacy cadence, preserved controls, production save/load/import/copy/restart and schema idempotence\n";
}
int main(int argc,char** argv) {
 VerifyDlssnrModeMigration();
 ScalingMode legacy; legacy.name=L"旧参数 α";
 legacy.effects={{.name=L"CAS\\CAS"},{.name=L"Color\\SDR_to_HDR",.parameters={{L"whiteNits",80.f},{L"exposure",.1f}}}};
 const auto original=Save(legacy); const auto manual=Load(original);
 assert(Save(manual)==original && manual.hdrMigrationBackup.empty() && !manual.hdrMigrationInvalid);
 assert(!manual.effects.back().parameters.contains(L"nativeWhiteNits"));
 auto native=Trial(1,2,R"([{"name":"CAS\\CAS"},{"name":"Color\\SDR_to_HDR","hdrManagedRole":3,"parameters":{"mode":1,"nativeWhiteNits":1500}}])");
 auto migrated=Load(Encode(native)); VerifyMigration(native,migrated);
 assert(!migrated.legacyHdrCaptureIntent && migrated.effects.back().parameters.at(L"nativeWhiteNits")==1500);
 rapidjson::Document backup; backup.Parse(migrated.hdrMigrationBackup.c_str());
 assert(backup["hdr"]["rtxPeakNits"].GetInt()==1701); // inactive preferences stay recoverable without extra nodes
 auto pair=Trial(2,0,R"([
 {"name":"Color\\HDR_to_SDR","hdrManagedRole":1,"parameters":{"mode":0,"whiteNits":203,"peakNits":1000,"exposure":1,"shoulder":1}},
 {"name":"CAS\\CAS","parameters":{"sharpness":0.731}},
 {"name":"Color\\SDR_to_HDR","hdrManagedRole":2,"parameters":{"mode":2}},
 {"name":"DLSS\\DLSS_SR","scalingType":1,"scale":{"x":1.2,"y":1.3}},
 {"name":"Color\\HDR_to_SDR","hdrManagedRole":1,"parameters":{"mode":0,"whiteNits":203,"peakNits":1000,"exposure":1,"shoulder":1}},
 {"name":"CAS\\CAS"},
 {"name":"Color\\SDR_to_HDR","hdrManagedRole":2,"parameters":{"mode":2}},
 {"name":"XeSSFG\\XeSS_FrameGeneration","parameters":{"multiplier":2}}
 ])");
 migrated=Load(Encode(pair)); VerifyMigration(pair,migrated);
 auto plan=HdrManualMigration::Plan(migrated.effects,migrated.legacyHdrCaptureIntent);
 assert(!migrated.legacyHdrCaptureIntent && plan.captureHdr && plan.outputHdr);
 assert(plan.stages[2].pairIndex==0 && plan.stages[6].pairIndex==4);
 migrated.effects[0].parameters[L"whiteNits"]=257.3f;
 migrated.effects[0].parameters[L"exposure"]=1.17f;
 auto edited=Load(Save(migrated)); assert(edited.effects[4].parameters.at(L"whiteNits")==203);
 const auto newPlan=HdrManualMigration::Plan(edited.effects,edited.legacyHdrCaptureIntent);
 assert(newPlan.stages[0].whiteNits==257.3f && newPlan.stages[4].whiteNits==203 && newPlan.stages[2].pairIndex==0 && newPlan.stages[6].pairIndex==4);
 // Materialized list stays authoritative after user add/remove/reorder/scale edits, even if they temporarily make it invalid.
 edited.effects.insert(edited.effects.begin()+1,{.name=L"Color\\Gamma",.parameters={{L"gamma",1.11f}}});
 auto saved=Save(edited); assert(Save(Load(saved))==saved && Load(saved).effects.size()==9);
 edited.effects.erase(edited.effects.begin()+1); std::swap(edited.effects[0],edited.effects[1]);
 edited.effects[3].scale={2.f,3.f}; saved=Save(edited); assert(Save(Load(saved))==saved && Load(saved).effects.size()==8);
 auto direct=Trial(2,0,R"([{"name":"Bicubic","hdrManagedRole":4},{"name":"DLSS\\DLSS_SR","parameters":{"quality":2}}])");
 migrated=Load(Encode(direct)); VerifyMigration(direct,migrated);
 assert(migrated.legacyHdrCaptureIntent==true && migrated.effects.size()==2);
 plan=HdrManualMigration::Plan(migrated.effects,migrated.legacyHdrCaptureIntent);
 assert(plan.enabled && plan.captureHdr && plan.outputHdr && plan.stages[0].inputHdr && plan.stages[1].outputHdr);
 const auto startup=FullImport(direct,1); assert(Save(startup)==Save(migrated));
 rapidjson::Document reopened; reopened.Parse(Save(startup).c_str());
 assert(Save(FullImport(reopened,0))==Save(startup)); // no repeated migration writes on restart
 auto ordinary=Trial(1,4,R"([{"name":"DLSS\\DLSS_SR"}])"); ordinary.RemoveMember("hdr");
 assert(FullImport(ordinary,1).effects[0].parameters.contains(L"opticalFlowMethod")); // normal migrations still run
 auto sdr=Trial(1,4,R"([{"name":"DLSSNR\\DLSSNR_AI_Filter"},{"name":"XeSSFG\\XeSS_FrameGeneration"}])");
 migrated=Load(Encode(sdr)); VerifyMigration(sdr,migrated);
 assert(migrated.legacyHdrCaptureIntent==false); // retain enabled SDR capture execution, not just final pixels
 std::vector<ScalingMode> modes{migrated,manual}; uint32_t defaultsVersion=0;
 const auto prior=Save(modes[0]); assert(ApplyOpticalFlowDefaultsMigration(modes,defaultsVersion));
 assert(defaultsVersion==1 && Save(modes[0])==prior);
 auto display=Trial(2,4,R"([{"name":"Color\\HDR_to_SDR","hdrManagedRole":1,"parameters":{"mode":1,"whiteNits":203,"peakNits":1000,"exposure":1,"shoulder":1}},{"name":"CAS\\CAS"}])");
 migrated=Load(Encode(display)); VerifyMigration(display,migrated);
 assert(!migrated.legacyHdrCaptureIntent && !HdrManualMigration::Plan(migrated.effects,{}).outputHdr);
 auto rtx=Trial(1,3,R"([{"name":"CAS\\CAS"},{"name":"RTXVideo\\RTXVideo_HDR","hdrManagedRole":3,"parameters":{"peakNits":1701,"contrast":133,"saturation":101,"middleGray":51}}])");
 migrated=Load(Encode(rtx)); VerifyMigration(rtx,migrated); assert(migrated.effects.size()==2);
 for (const auto [white,exposure]:{std::pair{80.f,.1f},std::pair{400.f,4.f}}) {
  auto low=Trial(1,2,R"([{"name":"Color\\SDR_to_HDR","hdrManagedRole":3,"parameters":{"mode":1,"whiteNits":80,"exposure":0.1,"peakNits":1000}}])");
  low["effects"][0]["parameters"]["whiteNits"].SetFloat(white); low["effects"][0]["parameters"]["exposure"].SetFloat(exposure);
  low["hdr"]["nativeLegacy"].SetBool(true); low["hdr"]["legacyWhiteNits"].SetFloat(white); low["hdr"]["legacyExposure"].SetFloat(exposure);
  low["hdr"]["nativeWhiteNits"].SetFloat(white*exposure);
  migrated=Load(Encode(low)); VerifyMigration(low,migrated);
  plan=HdrManualMigration::Plan(migrated.effects,{});
  assert(plan.stages[0].whiteNits==white && plan.stages[0].exposure==exposure && plan.stages[0].peakNits==1000);
  assert(!migrated.effects[0].parameters.contains(L"nativeWhiteNits"));
 }
 for (int kind=0;kind<9;++kind) {
  rapidjson::Document wrong; wrong.CopyFrom(native,wrong.GetAllocator());
  switch (kind) {
  case 0: wrong["hdr"]["version"].SetUint(2); break;
  case 1: wrong["effects"][1]["hdrManagedRole"].SetUint(256); break;
  case 2: wrong["hdr"]["nativeWhiteNits"].SetInt(1000); break;
  case 3: wrong["effects"][0].SetInt(37); break;
  case 4: wrong["effects"][1]["parameters"]["nativeWhiteNits"].SetString("invalid"); break;
  case 5: wrong.RemoveMember("hdr"); break;
  case 6: wrong["effects"][0].AddMember("hdrManagedRole",1,wrong.GetAllocator()); break;
  case 7: wrong["effects"][1]["parameters"]["mode"].SetInt(2); break;
  case 8: wrong.RemoveMember("effects"); break;
  }
  VerifyRejected(wrong);
 }
 // Backup version and simultaneous old/new schema corruption preserve their entire object.
 migrated=Load(Encode(native)); rapidjson::Document corrupt; corrupt.Parse(Save(migrated).c_str());
 corrupt["hdrManualMigration"]["version"].SetInt(2); VerifyRejected(corrupt);
 corrupt.Parse(Save(migrated).c_str()); corrupt["hdrManualMigration"]["original"].SetString("{bad"); VerifyRejected(corrupt);
 corrupt.Parse(Save(migrated).c_str()); corrupt["hdrManualMigration"]["original"].SetString("{}"); VerifyRejected(corrupt);
 corrupt.Parse(Save(Load(Encode(direct))).c_str()); corrupt["hdrManualMigration"]["captureHdr"].SetBool(false); VerifyRejected(corrupt);
 auto brokenManual=Trial(1,2,R"([{"name":"Color\\SDR_to_HDR","parameters":{"nativeWhiteNits":"invalid"}}])");
 brokenManual.RemoveMember("hdr"); VerifyRejected(brokenManual);
 assert(FullImport(brokenManual,0).hdrMigrationInvalid);
 // A renamed/duplicated invalid group keeps its own new name while retaining the unreadable fields.
 native["hdr"]["version"].SetInt(7); auto invalid=Load(Encode(native)); invalid.name=L"恢复副本";
 auto renamed=Load(Save(invalid)); assert(renamed.name==invalid.name && renamed.hdrMigrationInvalid);
 for (int i=1;i<argc;++i) {
  std::ifstream stream(argv[i],std::ios::binary); if (!stream) { std::cerr << "Could not open trial configuration: " << argv[i] << "\n"; return 2; }
  std::stringstream buffer; buffer<<stream.rdbuf();
  rapidjson::Document local; local.Parse(buffer.str().c_str()); assert(!local.HasParseError());
  const auto loaded=Load(Encode(local)); VerifyMigration(local,loaded);
  std::cout << "PASS: read-only local trial configuration migration\n";
 }
 std::cout << "PASS: production manual migration, unchanged legacy/Unicode, active/inactive preferences, independent pairs, direct HDR/SDR, FG/scales/order, low brightness, malformed metadata/ownership, raw recovery, imports and idempotent edits\n";
 return 0;
}
}
int main(int argc,char** argv) { return Magpie::main(argc,argv); }
'''
(out / 'HdrManualMigrationTests.cpp').write_text(prefix + recovery_rules + helper + writer + loader + normalizer + importer + tests,encoding='utf-8')
