"""Extract natural-size single-page XAML and real effect metadata for offscreen tests."""
from copy import deepcopy
from pathlib import Path
import json, re, sys
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[1]; output=Path(sys.argv[1])
p='{http://schemas.microsoft.com/winfx/2006/xaml/presentation}'; x='{http://schemas.microsoft.com/winfx/2006/xaml}'; local='{using:Magpie}'
ET.register_namespace('',p[1:-1]); ET.register_namespace('x',x[1:-1])
page=ET.parse(root/'src/Magpie/ScalingModesPage.xaml').getroot()
template=next(n for n in page.iter(p+'DataTemplate') if n.get(x+'Key')=='EffectParametersFlyout')
assert not list(template.iter(p+'ScrollViewer')) and not list(template.iter(p+'Viewbox'))
assert 'RowHeight' not in ET.tostring(template,encoding='unicode')
row=next(n for n in template.iter(p+'DataTemplate') if n.get(x+'DataType')=='local:ScalingModeParameter')[0]
group=next(n for n in template.iter(p+'DataTemplate') if n.get(x+'DataType')=='local:EffectParameterGroupViewModel')[0]
context=next(n for n in template.iter(local+'SimpleStackPanel') if 'HasOKContextTools' in n.get('Visibility',''))
common=next(n for n in template.iter(local+'SimpleStackPanel') if 'IsOKLab' in n.get('Visibility',''))
events={'Loaded','Unloaded','LostFocus','DataContextChanged','ValueChanged','KeyDown','PointerWheelChanged','DropDownOpened','DropDownClosed','SelectionChanged','SizeChanged','PointerPressed','PointerMoved','PointerReleased','PointerCanceled','PointerCaptureLost','Click'}
def literal(n): return 'LR"XAML('+ET.tostring(n,encoding='unicode')+')XAML"'
def text(v): return 'L'+json.dumps(v,ensure_ascii=False)
def clean(node,resources,state):
 node=deepcopy(node)
 for child in node.iter():
  uid=child.attrib.pop(x+'Uid',None)
  if uid:
   for key,value in resources.items():
    if key.startswith(uid+'/'): child.set(key[len(uid)+1:],value)
  if child.tag==local+'SimpleStackPanel': child.tag=p+'StackPanel'; child.set('Tag','ProductionSimpleStackPanel')
  for key,value in list(child.attrib.items()):
   if key in events: del child.attrib[key]
   elif value.startswith(('{x:Bind','{Binding')):
    prefix='{Binding ' if value.startswith('{Binding') else '{x:Bind '
    name=value[len(prefix):].split(',')[0].rstrip('}')
    if name in state:
     value=state[name]
     if key in ('Visibility',x+'Load'): value='Visible' if value else 'Collapsed'
     elif isinstance(value,bool): value='True' if value else 'False'
     if key==x+'Load':
      del child.attrib[key]; child.set('Visibility',str(value))
     else: child.set(key,str(value))
    elif key in ('Visibility',x+'Load'):
     if key==x+'Load': del child.attrib[key]
     child.set('Visibility','Collapsed')
    else: del child.attrib[key]
  if child.tag==p+'ComboBox' and 'PlaceholderText' not in child.attrib: child.set('PlaceholderText','Selection')
  if child.tag in (p+'Button',p+'ToggleButton',p+'CheckBox') and 'Content' in child.attrib:
   label=child.attrib.pop('Content'); ET.SubElement(child,p+'TextBlock',{'Text':label})
  if child.tag==p+'ComboBox' and 'Header' in child.attrib:
   label=child.attrib.pop('Header'); header=ET.SubElement(child,p+'ComboBox.Header'); ET.SubElement(header,p+'TextBlock',{'Text':label})
 return node
lines=['struct ParameterFixture { const char* name; const wchar_t* group; const wchar_t* xml; float value; };']
host=next(n for n in template.iter(p+'ItemsControl') if 'Groups' in n.get('ItemsSource',''))
host_fixture=ET.Element(p+'ItemsControl', {k:v for k,v in host.attrib.items() if not v.startswith('{x:Bind')})
for child in host:
 if child.tag==p+'ItemsControl.ItemContainerStyle': host_fixture.append(deepcopy(child))
item_template=ET.SubElement(host_fixture,p+'ItemsControl.ItemTemplate')
ET.SubElement(ET.SubElement(item_template,p+'DataTemplate'),p+'Grid',{'Width':'260','Height':'80'})
lines.append('static const wchar_t* GroupContainerFixture='+literal(host_fixture)+';')
for lang,language in enumerate(['zh-Hans','zh-Hant','en-US']):
 resources={n.get('name').replace('.','/'):n.findtext('value') for n in ET.parse(root/f'src/Magpie/Resources.language-{language}.resw').getroot().findall('data')}
 for effect,file in enumerate(['DLSSNR/DLSSNR_AI_Filter','OKLabImageAdjustment','CAS/CAS']):
  shader=(root/f'src/Effects/{file}.hlsl').read_text(encoding='utf-8-sig')
  entries=[]; blocks={}
  for block in shader.split('//!PARAMETER\n')[1:]:
   m=re.search(r'^(int|float) (\w+);',block,re.M)
   if not m: continue
   meta=dict(re.findall(r'^//!(\w+) (.*)$',block[:m.end()],re.M)); blocks[m[2]]=(m[1],meta)
  prefix='EffectParam_'+file.replace('/','_')+'_'
  for name,(kind,meta) in blocks.items():
   lookup=re.sub(r'^pass[23]_','',name) if effect==0 else name
   label=resources.get(prefix+lookup+'_Label',meta.get('LABEL',name))
   companion='uiCorrection' if name=='useAutoMask' else name[:6]+'uiCorrection' if name in ['pass2_useAutoMask','pass3_useAutoMask'] else 'performanceShowAdvanced' if name=='residualShowAdvanced' else ''
   companion_label=resources.get(prefix+re.sub(r'^pass[23]_','',companion)+'_Label','')
   boolean=kind=='int' and meta.get('MIN')=='0' and meta.get('MAX')=='1' and meta.get('STEP')=='1' and 'OPTION' not in meta
   choice='OPTION' in meta
   precise='INPUT_STEP' in meta
   state={'IsVisible':True,'IsEnabled':True,'IsCheckBox':boolean,'IsFloat':not boolean and not choice,'IsChoice':choice,'HasCompanion':bool(companion),'Companion.IsEnabled':True,'Companion.Label':companion_label,'Label':label,'PrimaryLabel':label.split('\n')[0],'DescriptionLabel':'\n'.join(label.split('\n')[1:]),'HasDescriptionLine':'\n' in label,'HasNoDescriptionLine':'\n' not in label,'IsPreciseInput':precise,'IsLegacyNumeric':not precise,'ValueText':meta.get('DEFAULT','0'),'EditableValue':meta.get('DEFAULT','0'),'SliderMinimum':0,'SliderMaximum':100,'SliderValue':20,'SliderStep':1,'Choices':None}
   state.pop('Choices')
   node=clean(row,resources,state)
   # Remove inactive branches, just as x:Bind Visibility does, while retaining
   # visible controls, wrapping, margins, fonts, editing widgets and tooltips.
   group_name=meta.get('GROUP','')
   group_label=resources.get(prefix+'Group_'+re.sub(r'[^A-Za-z0-9_]','_',group_name),group_name)
   entries.append('{'+json.dumps(name)+','+text(group_label)+','+literal(node)+','+str(float(meta.get('DEFAULT',0)))+'f}')
  lines.append(f'static const std::vector<ParameterFixture> Parameters{lang}_{effect}{{'+',\n'.join(entries)+'};')
 for section in range(-1,14):
  active={'IsOKLab':section>=0,'HasValidationMessage':False,'HasOKContextTools':section in [3,4,5,8,13],'OKMixerVisible':section==3,'OKWheelVisible':section==4,'OKSelectionVisible':section==5,'OKCurveVisible':section==8,'OKLutVisible':section==13,'OKStatus':'','OKWheelX':76,'OKWheelY':76}
  shell=deepcopy(template[0]); shell.remove(shell.find(p+'Grid'))
  body=ET.SubElement(shell,p+'Grid',{'Grid.Row':'1',x+'Name':'Body'})
  lines.append(f'static const wchar_t* Shell{lang}_{section+1}='+literal(clean(shell,resources,active))+';')
  lines.append(f'static const wchar_t* Context{lang}_{section+1}='+literal(clean(context,resources,active))+';')
 public=deepcopy(common)
 lines.append(f'static const wchar_t* Common{lang}='+literal(clean(public,resources,{'IsOKLab':True}))+';')
lines.append('static const std::array<std::array<std::vector<ParameterFixture>,3>,3> Fixtures{{'+','.join('{'+','.join(f'Parameters{l}_{e}' for e in range(3))+'}' for l in range(3))+'}};')
for label in ['Shell','Context']:
 lines.append('static const std::array<std::array<const wchar_t*,15>,3> '+label+'s{{'+','.join('{'+','.join(f'{label}{l}_{s}' for s in range(15))+'}' for l in range(3))+'}};')
lines.append('static const std::array<const wchar_t*,3> Commons{Common0,Common1,Common2};')
group=deepcopy(group)
inner=group.find(p+'Grid'); items=inner.find(p+'ItemsControl'); inner.remove(items)
ET.SubElement(inner,p+'Grid',{'Grid.Row':'1',x+'Name':'Rows'})
node=clean(group,{}, {'IsVisible':True,'HasLabel':True,'Label':'Group'})
next(n for n in node.iter(p+'TextBlock') if n.get('Text')=='Group').set(x+'Name','Title')
lines.append('static const wchar_t* GroupShell='+literal(node)+';')
source=(root/'src/Magpie/ScalingModesPage.cpp').read_text(encoding='utf-8-sig'); a=source.index('void ScalingModesPage::ParameterCheckboxRow_SizeChanged('); b=source.index('\n}\n',a)+2
lines.append(source[a:b].replace('ScalingModesPage::',''))
(output/'ParameterSinglePageProduction.inc').write_text('\n'.join(lines),encoding='utf-8')
source=(root/'src/Magpie/EffectParametersViewModel.cpp').read_text(encoding='utf-8-sig'); a=source.index('void EffectParametersViewModel::_RefreshVisibleGroups()'); b=source.index('// 应确保被删除后',a)
(output/'ParameterSinglePageStateProduction.inc').write_text(source[a:b],encoding='utf-8')
