#include "pch.h"
#include "EffectParametersViewModel.h"
#include "EffectChoiceItems.h"
#include "OKLabParameters.h"
#include "OKLabLut.h"
#include "App.h"
#include "AppSettings.h"
#include "CommonSharedConstants.h"
#include "EffectsService.h"
#include "ScalingMode.h"
#include "ScalingModesService.h"
#include "ScalingService.h"
#include "FileDialogHelper.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <robuffer.h>
#include "OKLabColor.h"

using namespace Magpie;
namespace winrt::Magpie::implementation {
namespace {
hstring OKText(std::wstring_view key) {
    return ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID).GetString(hstring(key));
}
IVector<hstring> OKList(std::wstring_view prefix,int count) {
    std::vector<hstring> items;
    for(int i=0;i<count;++i)items.push_back(OKText(std::wstring(prefix)+std::to_wstring(i)));
    return ::Magpie::MakeEffectChoiceItems(std::move(items));
}
}
bool EffectParametersViewModel::IsOKLab() const noexcept {
    return !_IsRemoved() && _effectInfo && _effectInfo->name==L"OKLabImageAdjustment";
}
IVector<hstring> EffectParametersViewModel::OKSections() const {
    std::vector<hstring> items;
    for(const auto section:OKLab::Sections)items.push_back(OKText(L"EffectParam_OKLabImageAdjustment_Group_"+StrHelper::UTF8ToUTF16(section)));
    return ::Magpie::MakeEffectChoiceItems(std::move(items));
}
IVector<hstring> EffectParametersViewModel::OKColors() const {return OKList(L"OKLab_Color_",8);}
IVector<hstring> EffectParametersViewModel::OKSlots() const {return ::Magpie::MakeEffectChoiceItems({L"A",L"B"});}
IVector<hstring> EffectParametersViewModel::OKCurves() const {return OKList(L"OKLab_Curve_",6);}
IVector<hstring> EffectParametersViewModel::OKPresets() const {return OKList(L"OKLab_Preset_",6);}
IVector<hstring> EffectParametersViewModel::OKWheels() const {return OKList(L"OKLab_Wheel_",3);}
void EffectParametersViewModel::_OKNotifyVisuals(){for(auto key:{L"OKCurvePoints",L"OKCurveGeometry",L"OKWheelX",L"OKWheelY"})RaisePropertyChanged(key);}
void EffectParametersViewModel::_OKRefreshSourceGroup() {
    // Publish the selected semantic section; XAML wraps its natural-size rows
    // into adjacent columns without changing the parameter objects.
    std::vector<IInspectable> parameters;
    const auto label=OKText(L"EffectParam_OKLabImageAdjustment_Group_"+
        StrHelper::UTF8ToUTF16(OKLab::Sections[_okSection]));
    for(const auto& parameter:_parameterImpls) {
        if(!parameter->IsVisible())continue;
        parameters.push_back(*parameter);
    }
    _sourceGroups.clear();
    _sourceGroups.push_back(make_self<EffectParameterGroupViewModel>(label,std::move(parameters)));
}
void EffectParametersViewModel::_OKRefreshEditor() {
    _RefreshConditionalVisibility();_OKRefreshSourceGroup();_RefreshVisibleGroups();
    for(auto key:{L"OKSection",L"OKColor",L"OKSlot",L"OKCurve",L"OKWheel",L"OKMixerVisible",L"OKSelectionVisible",L"OKCurveVisible",L"OKWheelVisible",L"OKLutVisible",L"HasOKContextTools"})RaisePropertyChanged(key);
    _OKNotifyVisuals();
}
// A ComboBox temporarily clears selection while its items/visuals reload.
// Treat -1 as no selection, not as a request to select the first item.
void EffectParametersViewModel::OKSection(int32_t value){if(!IsOKLab() || value<0)return;value=std::clamp(value,0,13);if(_okSection==value)return;_okSection=value;_okDragPoint=-1;_okWheelDragging=false;_OKRefreshEditor();}
void EffectParametersViewModel::OKColor(int32_t value){if(!IsOKLab() || value<0)return;value=std::clamp(value,0,7);if(_okColor==value)return;_okColor=value;_OKRefreshEditor();}
void EffectParametersViewModel::OKSlot(int32_t value){if(!IsOKLab() || value<0)return;value=std::clamp(value,0,1);if(_okSlot==value)return;_okSlot=value;_OKRefreshEditor();}
void EffectParametersViewModel::OKCurve(int32_t value){if(!IsOKLab() || value<0)return;value=std::clamp(value,0,5);if(_okCurve==value)return;_okCurve=value;_okDragPoint=-1;_OKRefreshEditor();}
void EffectParametersViewModel::OKWheel(int32_t value){if(!IsOKLab() || value<0)return;value=std::clamp(value,0,2);if(_okWheel==value)return;_okWheel=value;_okWheelDragging=false;_OKRefreshEditor();}
std::vector<float> EffectParametersViewModel::_OKSnapshot() const {
    std::vector<float> result;result.reserve(std::size(OKLab::Parameters));
    if(!IsOKLab())return result;
    for(const auto&p:OKLab::Parameters){const auto it=_Data().find(StrHelper::UTF8ToUTF16(p.name));result.push_back(OKLab::Normalize(p,it==_Data().end()?p.value:it->second));}
    return result;
}
void EffectParametersViewModel::_OKApply(const std::vector<float>& values,bool remember) {
    if(!IsOKLab() || values.size()!=std::size(OKLab::Parameters))return;
    auto before=_OKSnapshot();if(remember)_okUndo=before;
    for(size_t i=0;i<values.size();++i){
        const auto&p=OKLab::Parameters[i];const float value=OKLab::Normalize(p,values[i]);
        if(value==before[i])continue;
        _Data()[StrHelper::UTF8ToUTF16(p.name)]=value;
        ScalingService::Get().EffectParameterEdited(_scalingModeIdx,_effectIdx,std::string(p.name),value);
    }
    _SynchronizeParameters();
    ScalingModesService::Get().EffectParametersChanged.Invoke(_scalingModeIdx,_effectIdx);
    _LazySaveAppSettings();
}
void EffectParametersViewModel::OKApplyPreset(){
    auto values=_OKSnapshot();if(values.empty())return;
    OKLab::Preset(_okPreset,[&](std::string_view name,float value){values[OKLab::Find(name)-OKLab::Parameters]=value;});
    _OKApply(values);
}
void EffectParametersViewModel::OKResetGroup(){
    auto values=_OKSnapshot();if(values.empty())return;
    OKLab::Defaults([&](std::string_view name,float value){values[OKLab::Find(name)-OKLab::Parameters]=value;},OKLab::Sections[_okSection]);
    _OKApply(values);
}
void EffectParametersViewModel::OKResetAll(){auto values=_OKSnapshot();if(values.empty())return;for(size_t i=0;i<values.size();++i)values[i]=OKLab::Parameters[i].value;_OKApply(values);}
void EffectParametersViewModel::OKStoreA(){_okA=_OKSnapshot();}
void EffectParametersViewModel::OKSwapAB(){auto current=_OKSnapshot();if(_okA.empty()){_okA=current;return;}_OKApply(_okA);_okA=std::move(current);}
void EffectParametersViewModel::OKUndo(){auto current=_OKSnapshot();if(_okUndo.empty())return;_OKApply(_okUndo,false);_okUndo=std::move(current);}
Media::PointCollection EffectParametersViewModel::OKCurvePoints() const {
    Media::PointCollection result;
    if(!IsOKLab())return result;
    float previousX=0,previousY=0,firstY=0;
    for(int i=0;i<8;++i){
        auto get=[&](char axis,float fallback){const auto key=L"curve"+std::to_wstring(_okCurve)+wchar_t(axis)+std::to_wstring(i);const auto it=_Data().find(key);return it==_Data().end()?fallback:it->second;};
        float x=i==0?0:i==7?1:std::clamp(get('x',i/7.f),previousX+.001f,1-(7-i)*.001f);
        float y=get('y',_okCurve==0?i/7.f:0);
        if(_okCurve==0)y=i==0?0:i==7?1:std::clamp(y,previousY,1.f);
        if(i==0)firstY=y;
        if(_okCurve>=1&&_okCurve<=3&&i==7)y=firstY;
        result.Append(Point{x*240, (1-(_okCurve==0?y:(y+1)*.5f))*140});
        previousX=x;previousY=y;
    }
    return result;
}
Media::Geometry EffectParametersViewModel::OKCurveGeometry() const {
    Media::GeometryGroup group;const auto points=OKCurvePoints();if(points.Size()==0)return group;
    Media::PathGeometry line;Media::PathFigure figure;figure.StartPoint(points.GetAt(0));
    Media::PolyLineSegment segment;segment.Points(points);figure.Segments().Append(segment);line.Figures().Append(figure);group.Children().Append(line);
    for(const auto point:points){Media::EllipseGeometry node;node.Center(point);node.RadiusX(3);node.RadiusY(3);group.Children().Append(node);}
    return group;
}
void EffectParametersViewModel::OKCurvePressed(IInspectable const& sender,Input::PointerRoutedEventArgs const& args) {
    if(!OKCurveVisible())return;
    auto canvas=sender.as<Controls::Canvas>();auto p=args.GetCurrentPoint(canvas).Position();
    auto points=OKCurvePoints();float best=std::numeric_limits<float>::max();
    for(uint32_t i=0;i<points.Size();++i){auto q=points.GetAt(i);float d=(q.X-p.X)*(q.X-p.X)+(q.Y-p.Y)*(q.Y-p.Y);if(d<best){best=d;_okDragPoint=int(i);}}
    _okUndo=_OKSnapshot();canvas.CapturePointer(args.Pointer());OKCurveMoved(sender,args);args.Handled(true);
}
void EffectParametersViewModel::OKCurveMoved(IInspectable const& sender,Input::PointerRoutedEventArgs const& args) {
    if(_okDragPoint<0 || !OKCurveVisible())return;
    auto canvas=sender.as<Controls::Canvas>();auto point=args.GetCurrentPoint(canvas).Position();
    auto values=_OKSnapshot();const auto prefix="curve"+std::to_string(_okCurve);
    auto get=[&](std::string name)->float&{return values[OKLab::Find(name)-OKLab::Parameters];};
    const auto points=OKCurvePoints();
    for(int i=0;i<8;++i){const auto p=points.GetAt(i);get(prefix+"x"+std::to_string(i))=p.X/240;get(prefix+"y"+std::to_string(i))=_okCurve==0?1-p.Y/140:1-p.Y/70;}
    float x=std::clamp(point.X/240,0.f,1.f),y=std::clamp(1-point.Y/140,0.f,1.f);
    if(_okCurve!=0)y=y*2-1;
    const int k=_okDragPoint;
    if(k==0)x=0;else if(k==7)x=1;else x=std::clamp(x,get(prefix+"x"+std::to_string(k-1))+.001f,get(prefix+"x"+std::to_string(k+1))-.001f);
    if(_okCurve==0){if(k==0)y=0;else if(k==7)y=1;else y=std::clamp(y,get(prefix+"y"+std::to_string(k-1)),get(prefix+"y"+std::to_string(k+1)));}
    get(prefix+"Enabled")=1;get(prefix+"x"+std::to_string(k))=x;get(prefix+"y"+std::to_string(k))=y;
    if(_okCurve>=1&&_okCurve<=3&&(k==0||k==7)){get(prefix+"y0")=y;get(prefix+"y7")=y;}
    _OKApply(values,false);args.Handled(true);
}
void EffectParametersViewModel::OKCurveReleased(IInspectable const& sender,Input::PointerRoutedEventArgs const& args){_okDragPoint=-1;sender.as<Controls::Canvas>().ReleasePointerCapture(args.Pointer());}
Media::ImageSource EffectParametersViewModel::OKWheelImage(){
    if(_okWheelImage)return _okWheelImage;
    Media::Imaging::WriteableBitmap bitmap(160,160);
    uint8_t* bytes=nullptr;check_hresult(bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
    for(int y=0;y<160;++y)for(int x=0;x<160;++x){
        const float a=(x+.5f-80)/80,b=(80-y-.5f)/80;
        const auto rgb=OKLab::PreviewColor(a*.1f,b*.1f);
        const float alpha=std::clamp((1-std::hypot(a,b))*80,0.f,1.f);
        const size_t offset=(y*160+x)*4;
        bytes[offset]=uint8_t(std::round(rgb[2]*alpha*255));bytes[offset+1]=uint8_t(std::round(rgb[1]*alpha*255));
        bytes[offset+2]=uint8_t(std::round(rgb[0]*alpha*255));bytes[offset+3]=uint8_t(std::round(alpha*255));
    }
    bitmap.Invalidate();_okWheelImage=bitmap;return _okWheelImage;
}
double EffectParametersViewModel::OKWheelX() const {
    if(!IsOKLab())return 76;
    auto get=[&](std::wstring_view suffix){auto it=_Data().find(L"wheel"+std::to_wstring(_okWheel)+std::wstring(suffix));return it==_Data().end()?0.f:it->second;};
    return 76+std::cos(get(L"Hue")*.01745329252)*std::clamp(get(L"Chroma"),0.f,.1f)*800;
}
double EffectParametersViewModel::OKWheelY() const {
    if(!IsOKLab())return 76;
    auto get=[&](std::wstring_view suffix){auto it=_Data().find(L"wheel"+std::to_wstring(_okWheel)+std::wstring(suffix));return it==_Data().end()?0.f:it->second;};
    return 76-std::sin(get(L"Hue")*.01745329252)*std::clamp(get(L"Chroma"),0.f,.1f)*800;
}
void EffectParametersViewModel::OKWheelPressed(IInspectable const& sender,Input::PointerRoutedEventArgs const& args){
    if(!OKWheelVisible())return;_okUndo=_OKSnapshot();_okWheelDragging=true;sender.as<Controls::Canvas>().CapturePointer(args.Pointer());OKWheelMoved(sender,args);args.Handled(true);
}
void EffectParametersViewModel::OKWheelMoved(IInspectable const& sender,Input::PointerRoutedEventArgs const& args){
    if(!_okWheelDragging||!OKWheelVisible())return;
    const auto point=args.GetCurrentPoint(sender.as<Controls::Canvas>()).Position();auto values=_OKSnapshot();
    const auto prefix="wheel"+std::to_string(_okWheel);
    const auto hueIndex=OKLab::Find(prefix+"Hue")-OKLab::Parameters,chromaIndex=OKLab::Find(prefix+"Chroma")-OKLab::Parameters;
    const auto selected=OKLab::WheelValue((point.X-80)/80,(80-point.Y)/80,values[hueIndex]);
    values[hueIndex]=selected[0];values[chromaIndex]=selected[1];_OKApply(values,false);args.Handled(true);
}
void EffectParametersViewModel::OKWheelReleased(IInspectable const& sender,Input::PointerRoutedEventArgs const& args){_okWheelDragging=false;sender.as<Controls::Canvas>().ReleasePointerCapture(args.Pointer());}
fire_and_forget EffectParametersViewModel::OKImportLut() {
    if(!IsOKLab())co_return;
    auto weak=get_weak();auto identity=ScalingModesService::Get().GetScalingMode(_scalingModeIdx).effects[_effectIdx].runtimeIdentity;
    co_await resume_background();
    com_ptr<IFileOpenDialog> dialog=try_create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);if(!dialog)co_return;
    const COMDLG_FILTERSPEC filter{L"3D LUT (*.cube)",L"*.cube"};dialog->SetFileTypes(1,&filter);
    auto path=FileDialogHelper::OpenFileDialog(dialog.get(),FOS_FILEMUSTEXIST);if(!path||path->empty())co_return;
    std::string text,error;OKLab::Cube cube;std::error_code ec;
    auto size=std::filesystem::file_size(*path,ec);
    if(ec||size>16*1024*1024)error="LUT file must be at most 16 MiB";
    else if(!Win32Helper::ReadTextFile(path->c_str(),text))error="Could not read LUT";
    else if(!OKLab::ParseCube(text,cube,error))text.clear();
    co_await App::Get().Dispatcher();auto self=weak.get();if(!self||!self->IsOKLab())co_return;
    auto& item=ScalingModesService::Get().GetScalingMode(self->_scalingModeIdx).effects[self->_effectIdx];
    if(item.runtimeIdentity!=identity)co_return;
    if(error.empty()){item.oklabLut=std::move(text);self->_okStatus=OKText(L"OKLab_LutLoaded");_LazySaveAppSettings();}
    else self->_okStatus=to_hstring(error);
    self->RaisePropertyChanged(L"OKStatus");
}
fire_and_forget EffectParametersViewModel::OKExportLut() {
    if(!IsOKLab())co_return;
    auto weak=get_weak();const auto source=ScalingModesService::Get().GetScalingMode(_scalingModeIdx).effects[_effectIdx].oklabLut;
    co_await resume_background();
    com_ptr<IFileSaveDialog> dialog=try_create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);if(!dialog)co_return;
    const COMDLG_FILTERSPEC filter{L"3D LUT (*.cube)",L"*.cube"};dialog->SetFileTypes(1,&filter);dialog->SetDefaultExtension(L"cube");dialog->SetFileName(L"OKLab LUT.cube");
    auto path=FileDialogHelper::OpenFileDialog(dialog.get());if(!path||path->empty())co_return;
    OKLab::Cube cube;std::string error;
    if(!source.empty())OKLab::ParseCube(source,cube,error);
    if(error.empty()&&!Win32Helper::WriteTextFile(path->c_str(),OKLab::ExportCube(cube)))error="Could not export LUT";
    co_await App::Get().Dispatcher();auto self=weak.get();if(!self)co_return;
    self->_okStatus=error.empty()?OKText(L"OKLab_LutSaved"):to_hstring(error);self->RaisePropertyChanged(L"OKStatus");
}
void EffectParametersViewModel::OKClearLut(){if(!IsOKLab())return;ScalingModesService::Get().GetScalingMode(_scalingModeIdx).effects[_effectIdx].oklabLut.clear();_okStatus=OKText(L"OKLab_LutRestart");RaisePropertyChanged(L"OKStatus");_LazySaveAppSettings();}
}
