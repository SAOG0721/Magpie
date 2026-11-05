"""Generate the effect metadata, parameter contract and translations together.

The numerical implementation lives in OKLab/Adjust.hlsli. Run --check in tests.
No third-party Python packages are required.
"""
from pathlib import Path
import argparse
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
PARAMS = []
GROUPS = [
    ('Basic', '基础'), ('Tone', '明暗'), ('Color', '颜色'),
    ('Mixer', '八色混合器'), ('Wheels', '三向调色'), ('Selection', '选色'),
    ('Style', '风格'), ('Spatial', '局部处理'), ('Curves', '曲线'),
    ('Mask', '几何蒙版'), ('Automatic', '自动调整'), ('Calibration', '白平衡与通道'),
    ('Output', '输出与检查'), ('Lut', 'LUT'),
]
EDITOR_LABELS = {
    'Section': ('Editing section', '编辑区域'), 'Color': ('Color range', '颜色范围'),
    'Slot': ('Selection slot', '选色槽'), 'Curve': ('Curve', '曲线'),
    'Preset': ('Preset', '预设'), 'ApplyPreset': ('Apply preset', '应用预设'),
    'ResetGroup': ('Reset section', '重置本区域'), 'ResetAll': ('Reset all', '全部重置'),
    'StoreA': ('Store A', '保存 A'), 'SwapAB': ('Swap A / B', '交换 A / B'),
    'Undo': ('Undo last edit', '撤销上次编辑'), 'ImportLut': ('Import .cube', '导入 .cube'),
    'ExportLut': ('Export LUT', '导出 LUT'), 'ClearLut': ('Clear LUT', '清除 LUT'),
    'LutRestart': ('LUT asset changes apply after restarting scaling.', 'LUT 文件变更在重新开始缩放后生效。'),
    'LutExportHelp': ('Export saves the loaded LUT, or an identity LUT when empty. It does not bake the image adjustments.', '导出当前加载的 LUT；未加载时导出中性 LUT，不烘焙图像调整参数。'),
    'CurveHelp': ('Drag one of eight points. Undo restores the previous edit.', '拖动八个节点调整曲线，可撤销上次编辑。'),
    'LutLoaded': ('LUT imported. Restart scaling to apply.', 'LUT 已导入，重新开始缩放后生效。'),
    'LutSaved': ('LUT exported.', 'LUT 已导出。'),
    'PickColor': ('Pick source color', '拾取输入颜色'),
    'PickHelp': ('Hold the pick button, drag onto the running image, then release. Escape cancels.', '按住拾色按钮，拖到运行画面后松开。Esc 取消。'),
    'PickSettingsHelp': ('Pick source colors from the running parameter panel.', '可在运行画面的参数浮窗中拾取输入颜色。'),
    'Wheel': ('Tone zone', '明暗区域'),
    'WheelHelp': ('Direction sets hue; distance from the center sets tint strength.', '方向控制色相，离中心的距离控制染色量。'),
    'Wheel_0': ('Shadows', '阴影'), 'Wheel_1': ('Midtones', '中间调'), 'Wheel_2': ('Highlights', '高光'),
}
for i,(en,zh) in enumerate([('Neutral','中性'),('Natural','自然增强'),('Soft highlights','柔和高光'),('Cool shadows / warm light','冷影暖光'),('Black and white','黑白'),('Vintage duotone','复古双色')]):
    EDITOR_LABELS[f'Preset_{i}']=(en,zh)


def add(group, name, en, zh, default=0, low=0, high=1, step=.01, options=None):
    PARAMS.append(dict(group=group, name=name, en=en, zh=zh, default=default,
                       low=low, high=high, step=step, options=options))


add('Basic', 'amount', 'Effect strength', '效果强度', 1)
add('Basic', 'exposure', 'Exposure (EV)', '曝光（EV）', 0, -4, 4)
add('Basic', 'lightness', 'Perceived lightness', '感知明度', 0, -.25, .25, .001)
add('Basic', 'contrast', 'Contrast', '对比度', 1, 0, 2)
add('Basic', 'gamma', 'Midtone gamma', '中间调 Gamma', 1, .25, 4)
add('Basic', 'shadows', 'Shadows', '阴影', 0, -1, 1)
add('Basic', 'highlights', 'Highlights', '高光', 0, -1, 1)
add('Basic', 'chroma', 'Chroma', '色度', 1, 0, 2.5)
add('Basic', 'vibrance', 'Vibrance', '自然饱和度', 0, -1, 1)
add('Basic', 'warmth', 'Warm / cool', '暖冷', 0, -1, 1)
add('Basic', 'tint', 'Green / magenta', '绿洋红', 0, -1, 1)
for name, en, zh, d, lo, hi in [
    ('pivot', 'Contrast pivot', '对比度支点', .5, .1, .9),
    ('blackIn', 'Input black', '输入黑场', 0, 0, .25),
    ('whiteIn', 'Input white', '输入白场', 1, .75, 1),
    ('blackOut', 'Output black', '输出黑场', 0, 0, .25),
    ('whiteOut', 'Output white', '输出白场', 1, .75, 1),
    ('midtones', 'Midtones', '中间调', 0, -1, 1),
    ('balance', 'Zone balance', '区域平衡', 0, -1, 1),
    ('zoneWidth', 'Zone transition', '区域过渡', .15, .05, .3),
    ('toe', 'Shadow softness', '暗部柔化', 0, 0, 1),
    ('shoulder', 'Highlight softness', '高光柔化', 0, 0, 1),
    ('darkProtect', 'Shadow protection', '暗部保护', 0, 0, 1),
    ('lightProtect', 'Highlight protection', '高光保护', 0, 0, 1),
    ('tone25', 'Curve at 25%', '曲线 25%', .25, 0, 1),
    ('tone50', 'Curve at 50%', '曲线 50%', .5, 0, 1),
    ('tone75', 'Curve at 75%', '曲线 75%', .75, 0, 1),
]:
    add('Tone', name, en, zh, d, lo, hi, .001)
add('Color', 'hue', 'Hue rotation (degrees)', '色相旋转（度）', 0, -180, 180, .1)
for name, en, zh in [('chromaLow', 'Low chroma', '低色度'),
                      ('chromaMid', 'Medium chroma', '中等色度'),
                      ('chromaHigh', 'High chroma', '高色度'),
                      ('shadowChroma', 'Shadow chroma', '阴影色度'),
                      ('midChroma', 'Midtone chroma', '中间调色度'),
                      ('highlightChroma', 'Highlight chroma', '高光色度')]:
    add('Color', name, en, zh, 1, 0, 2)
add('Color', 'neutralProtect', 'Neutral protection', '中性色保护')
add('Color', 'skinProtect', 'Skin color range protection', '肤色色域保护')
add('Color', 'skinHue', 'Protected hue', '保护色相', 50, 0, 360, .1)
add('Color', 'skinWidth', 'Protected hue width', '保护色相半宽', 35, 5, 90, .1)
add('Color', 'skinLightLow', 'Protected minimum lightness', '保护最低明度', .25)
add('Color', 'skinLightHigh', 'Protected maximum lightness', '保护最高明度', .9)
add('Color', 'skinChromaLow', 'Protected minimum chroma', '保护最低色度', .015, 0, .4, .001)
add('Color', 'skinChromaHigh', 'Protected maximum chroma', '保护最高色度', .2, 0, .4, .001)
add('Color', 'biasA', 'Cyan / red bias', '青红偏移', 0, -.05, .05, .001)
add('Color', 'biasB', 'Blue / yellow bias', '蓝黄偏移', 0, -.05, .05, .001)
COLORS = [('Red','红'), ('Orange','橙'), ('Yellow','黄'), ('Green','绿'),
          ('Cyan','青'), ('Blue','蓝'), ('Purple','紫'), ('Magenta','洋红')]
for i, (en, zh) in enumerate(COLORS):
    add('Mixer', f'mix{i}Hue', f'{en} hue', f'{zh}色色相', 0, -45, 45, .1)
    add('Mixer', f'mix{i}Chroma', f'{en} chroma', f'{zh}色色度', 1, 0, 2)
    add('Mixer', f'mix{i}Light', f'{en} lightness', f'{zh}色明度', 0, -.15, .15, .001)
add('Mixer', 'mixerFeather', 'Hue overlap', '色相重叠', .5)
for i, (en, zh) in enumerate([('Shadow','阴影'), ('Midtone','中间调'), ('Highlight','高光')]):
    add('Wheels', f'wheel{i}Hue', f'{en} hue', f'{zh}色相', 0, 0, 360, .1)
    add('Wheels', f'wheel{i}Chroma', f'{en} tint amount', f'{zh}染色量', 0, 0, .1, .001)
    add('Wheels', f'wheel{i}Light', f'{en} lightness', f'{zh}明度', 0, -.1, .1, .001)
for i in range(2):
    pre = f'sel{i}'
    slot = 'AB'[i]
    for name, en, zh, d, lo, hi, step in [
        ('Enabled','Enable','启用',0,0,1,1), ('Hue','Hue','色相',50,0,360,.1),
        ('Width','Hue half width','色相半宽',30,1,180,.1),
        ('CLow','Minimum chroma','最低色度',0,0,2,.001),
        ('CHigh','Maximum chroma','最高色度',2,0,2,.001),
        ('LLow','Minimum lightness','最低明度',0,0,5,.01),
        ('LHigh','Maximum lightness','最高明度',5,0,5,.01),
        ('Feather','Feather','羽化',.2,.01,1,.01), ('Invert','Invert','反转',0,0,1,1),
        ('DeltaHue','Hue shift','色相偏移',0,-180,180,.1),
        ('Chroma','Chroma','色度',1,0,2,.01), ('Light','Lightness','明度',0,-.25,.25,.001),
        ('TargetHue','Target hue','目标色相',50,0,360,.1),
        ('Unify','Hue unification','色相统一',0,0,1,.01),
    ]:
        add('Selection',pre+name,f'{slot}: {en}',f'{slot}：{zh}',d,lo,hi,step)
add('Selection', 'backgroundChroma', 'Outside selection chroma', '选区外色度', 1)
add('Style','styleMode','Style','风格',options=[('None','无'),('Black and white','黑白'),('Duotone','双色'),('Tritone','三色')])
add('Style','styleAmount','Style strength','风格强度',1)
for i, (en, zh) in enumerate(COLORS):
    add('Style', f'bw{i}', f'{en} gray mix', f'{zh}色灰阶', 0, -1, 1)
for i in range(3):
    add('Style',f'style{i}Hue',f'Color {i+1} hue',f'颜色 {i+1} 色相',[240,40,75][i],0,360,.1)
    add('Style',f'style{i}Chroma',f'Color {i+1} chroma',f'颜色 {i+1} 色度',.04,0,.15,.001)
add('Style','styleBalance','Style balance','风格平衡',.5,.1,.9)
for name,en,zh,d,lo,hi,step in [
    ('clarity','Local contrast','局部对比度',0,-1,1,.01),
    ('clarityRadius','Local contrast radius','局部对比度半径',12,1,64,1),
    ('edgeProtect','Edge protection','边缘保护',.1,.005,.5,.005),
    ('textureStrength','Texture','纹理',0,-1,1,.01),('textureRadius','Texture radius','纹理半径',2,1,8,1),
    ('noiseFloor','Noise floor','细节噪声阈值',.005,0,.1,.001),
    ('chromaSmooth','Chroma smoothing','色度平滑',0,0,1,.01),
    ('chromaRadius','Chroma radius','色度半径',3,1,16,1),
    ('deband','Debanding','去色带',0,0,1,.01),('debandRadius','Deband radius','去色带半径',8,1,32,1),
    ('debandThreshold','Deband threshold','去色带阈值',.01,.001,.05,.001),
    ('bloom','Bloom','高光辉光',0,0,1,.01),('bloomRadius','Bloom radius','辉光半径',24,1,128,1),
    ('bloomThreshold','Bloom threshold','辉光阈值',1,0,8,.01),('bloomSoftness','Bloom softness','辉光柔化',.2,.01,2,.01),
    ('dehaze','Dehaze','去雾',0,0,1,.01),('dehazeRadius','Dehaze radius','去雾半径',24,1,128,1),
    ('dehazeProtect','Dehaze color protection','去雾颜色保护',1,0,1,.01),
]:
    add('Spatial',name,en,zh,d,lo,hi,step)
CURVES=[('Lightness','明度'),('Hue to hue','色相到色相'),('Hue to chroma','色相到色度'),
        ('Hue to lightness','色相到明度'),('Lightness to chroma','明度到色度'),('Chroma to chroma','色度到色度')]
for c,(en,zh) in enumerate(CURVES):
    add('Curves',f'curve{c}Enabled',f'{en}: enable',f'{zh}：启用',0,0,1,1)
    for k in range(8):
        add('Curves',f'curve{c}x{k}',f'Point {k+1} input',f'节点 {k+1} 输入',k/7,0,1,.001)
        default=k/7 if c==0 else 0
        add('Curves',f'curve{c}y{k}',f'Point {k+1} output',f'节点 {k+1} 输出',default,0 if c==0 else -1,1,.001)
add('Mask','maskMode','Local mask','局部蒙版',options=[('Off','关闭'),('Linear gradient','线性渐变'),('Radial gradient','径向渐变')])
for name,en,zh,d,lo,hi in [
    ('maskX','Center X','中心 X',.5,0,1),('maskY','Center Y','中心 Y',.5,0,1),
    ('maskAngle','Angle','角度',0,-180,180),('maskWidth','Width','宽度',.5,.01,2),
    ('maskHeight','Height','高度',.5,.01,2),('maskFeather','Feather','羽化',.25,.001,1),
    ('maskInvert','Invert','反转',0,0,1)]:
    add('Mask',name,en,zh,d,lo,hi,1 if name=='maskInvert' else .01)
add('Automatic','autoExposure','Auto exposure','自动曝光',0,0,1,1)
add('Automatic','autoWhite','Auto white balance','自动白平衡',0,0,1,1)
add('Automatic','autoLock','Lock automatic adjustment','锁定自动调整',0,0,1,1)
add('Automatic','autoTarget','Target linear luminance','目标线性亮度',.18,.02,.8)
add('Automatic','autoLimit','Exposure limit (EV)','曝光限制（EV）',2,0,4)
add('Automatic','autoSpeed','Adaptation time (seconds)','适应时间（秒）',1,.05,10)
add('Automatic','sceneThreshold','Scene cut threshold (EV)','切镜阈值（EV）',2,.5,6)
add('Calibration','temperatureEnabled','Calibrated white balance','标定白平衡',0,0,1,1)
add('Calibration','sourceKelvin','Source white (K)','源白点（K）',6504,2000,25000,1)
add('Calibration','targetKelvin','Target white (K)','目标白点（K）',6504,2000,25000,1)
add('Calibration','whiteTint','Target white tint (delta y)','目标白点色偏（Δy）',0,-.05,.05,.0001)
for i,(en,zh) in enumerate([('Red','红'),('Green','绿'),('Blue','蓝')]):
    add('Calibration',f'gain{i}',f'{en} gain',f'{zh}色增益',1,0,2)
    for j,(src,sz) in enumerate([('red','红'),('green','绿'),('blue','蓝')]):
        add('Calibration',f'matrix{i}{j}',f'{en} from {src}',f'{zh}输出中的{sz}分量',1 if i==j else 0,-2,2)
add('Output','gamutSoft','Gamut compression','色域软压缩')
add('Output','referenceWhite','HDR reference white (nits)','HDR 参考白（尼特）',203,80,500,1)
add('Output','peakNits','HDR peak (nits)','HDR 峰值（尼特）',1000,500,10000,1)
add('Output','outputGamut','HDR output gamut','HDR 输出色域',2,options=[('Rec.709','Rec.709'),('Display P3','Display P3'),('Rec.2020','Rec.2020')])
add('Output','dither','Output dither','输出抖动')
add('Output','ditherBits','Output quantization','输出量化',0,options=[('8-bit','8 位'),('10-bit','10 位'),('16-bit','16 位')])
add('Output','view','Preview','预览',options=[('Normal','正常'),('Lightness','明度'),('Chroma','色度'),('Difference','调整差值'),('Out of gamut','色域越界'),('Compression','色域压缩量'),('Selection A','选区 A'),('Selection B','选区 B'),('Local mask','局部蒙版'),('Clipping','裁切预警'),('Grayscale','灰阶')])
add('Output','split','Before / after split','前后分屏',0,0,1,1)
add('Output','splitPosition','Split position','分屏位置',.5)
add('Lut','lutAmount','LUT strength','LUT 强度')
add('Lut','lutDomain','LUT input encoding','LUT 输入编码',options=[('sRGB','sRGB'),('Linear RGB','线性 RGB')])
DESCRIPTIONS={
    'amount':('Mixes the original and adjusted image. Zero restores the original pixels.','混合原图与调整结果，设为 0 恢复原始像素。'),
    'exposure':('One EV doubles or halves linear-light intensity.','增加 1 EV 使线性光强翻倍，降低 1 EV 则减半。'),
    'lightness':('Moves perceived lightness independently of exposure.','独立调整感知明度，与曝光分别控制。'),
    'gamma':('Higher values brighten midtones while retaining black and white endpoints.','增大时提亮中间调，并保留黑白端点。'),
    'pivot':('Sets the lightness around which contrast expands. Has no effect at contrast 1.','设置对比度伸缩的明度中心，对比度为 1 时不改变画面。'),
    'darkProtect':('Reduces tone adjustments in originally dark areas.','减弱明暗调整对原图暗部的影响。'),
    'lightProtect':('Reduces tone adjustments in originally bright areas.','减弱明暗调整对原图亮部的影响。'),
    'chroma':('Changes color intensity while retaining Oklab lightness. Zero removes color.','调整颜色浓度并保留 OKLab 明度；设为 0 去除颜色。'),
    'vibrance':('Emphasizes weaker colors and reduces the boost for already vivid colors.','更多增强中低色度颜色，减少对已有鲜艳颜色的提升。'),
    'neutralProtect':('Protects original neutrals from white balance, color bias and three-way tinting.','减弱白平衡、颜色偏移和三向调色对原始灰色区域的染色。'),
    'skinProtect':('Protects the configured color range from vibrance and the eight-color mixer.','保护指定肤色色域，减弱自然饱和度和八色混合器对其影响。'),
    'clarity':('Enhances broad lightness structure. High strength can create halos.','增强较大范围的明暗结构，过高可能产生光晕。'),
    'textureStrength':('Enhances fine lightness detail above the noise threshold.','增强高于噪声阈值的细小明暗纹理。'),
    'chromaSmooth':('Smooths color noise with protection across lightness edges.','平滑彩色噪点，并保护明暗边缘。'),
    'deband':('Smooths small differences in flat gradients. A high threshold may soften detail.','平滑渐变中的细小色阶差异，阈值过高可能减弱细节。'),
    'bloom':('Spreads bright colors around highlights. Radius and strength increase its reach.','将高亮颜色扩散到周围，半径与强度控制扩散范围。'),
    'dehaze':('Estimates local airlight and transmission from nearby pixels.','从邻近像素估计局部空气光与透射率，用于降低雾感。'),
    'autoExposure':('Adapts exposure to bounded average log luminance on captured frames.','根据捕获帧的平均对数亮度自动调整曝光。'),
    'autoWhite':('Balances average color. Lock it when a dominant scene color should be retained.','根据平均颜色修正偏色，需要保留场景主色调时可锁定。'),
    'autoLock':('Holds the current automatic estimate until unlocked.','保持当前自动调整估计，直到解除锁定。'),
    'autoSpeed':('Time constant in seconds; larger values change more slowly.','适应时间，单位为秒；值越大变化越慢。'),
    'whiteTint':('Offsets target white in CIE xy delta-y. Zero follows the Planckian-locus fit.','以 CIE xy 的 Δy 调整目标白点，0 使用黑体轨迹近似。'),
    'outputGamut':('Constrains adjusted HDR colors. SDR always maps to sRGB.','约束调整后的 HDR 颜色；SDR 始终映射到 sRGB。'),
    'referenceWhite':('HDR working reference white in nits; ordinary SDR is unchanged.','HDR 工作参考白的尼特值，不改变普通 SDR 的颜色合同。'),
    'dither':('Adds fixed pixel noise before output storage; HDR strength is relative to reference white.','在输出存储前加入固定像素噪声；HDR 强度按参考白定义。'),
    'lutAmount':('Mixes the imported cube LUT. Import and export are in the settings panel.','混合导入的 cube LUT，可在设置面板导入和导出文件。'),
    'lutDomain':('Use the encoding for which the LUT was authored. HDR excursions are retained outside its domain.','选择制作 LUT 时使用的输入编码，域外 HDR 亮度保留为额外分量。'),
}


def num(v):
    return format(float(v), '.9g')


def generated():
    header='// Generated by tools/generate_oklab_parameters.py.\n//!MAGPIE EFFECT\n//!VERSION 4\n\n'
    defs=[]
    for p in PARAMS:
        opts=p['options']
        integer=opts is not None or p['step']==1
        header+=f"//!PARAMETER\n//!GROUP {p['group']}\n//!LABEL {p['en']}\n//!DEFAULT {num(p['default'])}\n"
        if opts:
            header+=''.join(f'//!OPTION {i} {en}\n' for i,(en,_) in enumerate(opts))
            lo,hi=0,len(opts)-1
        else:
            lo,hi=p['low'],p['high']
            header+=f"//!MIN {num(lo)}\n//!MAX {num(hi)}\n//!STEP {num(p['step'])}\n"
        header+=f"{'int' if integer else 'float'} {p['name']};\n\n"
        defs.append(f'    {{"{p["name"]}", "{p["group"]}", float({num(p["default"])}), float({num(lo)}), float({num(hi)}), {str(integer).lower()}}},')
    header+=(ROOT/'tools/oklab_effect_passes.txt').read_text(encoding='utf-8')
    cpp='''// Generated by tools/generate_oklab_parameters.py.
#pragma once
#include <array>
#include <string_view>
namespace Magpie::OKLab {
struct Parameter { std::string_view name, group; float value, minimum, maximum; bool integer; };
inline constexpr Parameter Parameters[] = {
'''+ '\n'.join(defs)+ '\n};\n}\n'
    tables='// Generated parameter gathers. Algorithms are in Adjust.hlsli.\n'
    tables+='float OKUserCurve(float x,int curve) {\n'
    for c in range(6):
        tables+=f'    if(curve=={c}) {{\n'
        tables+=f'        if(curve{c}Enabled==0) return '+('x' if c==0 else '0')+';\n'
        tables+='        float xs[8]={'+','.join(f'curve{c}x{k}' for k in range(8))+'};\n'
        tables+='        float ys[8]={'+','.join(f'curve{c}y{k}' for k in range(8))+'};\n'
        tables+=f'        return OKCurve8(x,xs,ys,{str(c==0).lower()},{str(c in (1,2,3)).lower()});\n    }}\n'
    tables+='    return 0;\n}\n'
    tables+='float4 OKMixer(float3 original) {\n'
    for suffix,var in [('Hue','hs'),('Chroma','cs'),('Light','ls')]:
        tables+='    float '+var+'[8]={'+','.join(f'mix{i}{suffix}' for i in range(8))+'};\n'
    tables+='    float bs[8]={'+','.join(f'bw{i}' for i in range(8))+'};\n'
    tables+='    return OKMixBands(original,hs,cs,ls,bs);\n}\n'
    return {'src/Effects/OKLabImageAdjustment.hlsl':header,
            'src/Magpie.Core/include/OKLabParameterTable.h':cpp,
            'src/Effects/OKLab/Tables.hlsli':tables}


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    for path,text in generated().items():
        target=ROOT/path
        if args.check:
            assert target.read_text(encoding='utf-8-sig')==text, f'Stale generated file: {path}'
        else:
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_text(text,encoding='utf-8')
    for locale,lang in [('en-US','en'),('zh-Hans','zh'),('zh-Hant','zh')]:
        path=ROOT/f'src/Magpie/Resources.language-{locale}.resw'
        raw=path.read_text(encoding='utf-8-sig')
        data={f'EffectParam_OKLabImageAdjustment_Group_{en}': en if lang=='en' else zh for en,zh in GROUPS}
        data.update({f'OKLab_{key}':pair[0 if lang=='en' else 1] for key,pair in EDITOR_LABELS.items()})
        data.update({f'OKLab_Color_{i}':pair[0 if lang=='en' else 1] for i,pair in enumerate(COLORS)})
        data.update({f'OKLab_Curve_{i}':pair[0 if lang=='en' else 1] for i,pair in enumerate(CURVES)})
        data.update({f'EffectParam_OKLabImageAdjustment_{key}_Description':pair[0 if lang=='en' else 1] for key,pair in DESCRIPTIONS.items()})
        for key in ['ApplyPreset','ResetGroup','ResetAll','StoreA','SwapAB','Undo','ImportLut','ExportLut','ClearLut','PickColor']:
            data[f'OKLabUI_{key}/Content']=data[f'OKLab_{key}']
        for key in ['Section','Color','Slot','Curve','Preset','Wheel']:
            data[f'OKLabUI_{key}/Header']=data[f'OKLab_{key}']
        for key in ['CurveHelp','LutRestart','LutExportHelp','PickHelp','PickSettingsHelp','WheelHelp']:
            data[f'OKLabUI_{key}/Text']=data[f'OKLab_{key}']
        for p in PARAMS:
            data[f'EffectParam_OKLabImageAdjustment_{p["name"]}_Label']=p[lang]
            if p['options']:
                for i, (en,zh) in enumerate(p['options']):
                    data[f'EffectParam_OKLabImageAdjustment_{p["name"]}_Option_{i}']=en if lang=='en' else zh
        if locale=='zh-Hant':
            import ctypes
            kernel=ctypes.WinDLL('kernel32',use_last_error=True)
            convert=kernel.LCMapStringEx
            convert.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_wchar_p,ctypes.c_int,ctypes.c_wchar_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_longlong]
            for key,value in data.items():
                buffer=ctypes.create_unicode_buffer(len(value)*2+1)
                if not convert('zh-TW',0x04000000,value,len(value),buffer,len(buffer),None,None,0):
                    raise ctypes.WinError(ctypes.get_last_error())
                data[key]=buffer.value
        if args.check:
            existing={x.attrib['name']:x.findtext('value') for x in ET.fromstring(raw).findall('data')}
            for key in existing:
                parts=key.split('/')
                assert not any('/'.join(parts[:i]) in existing for i in range(1,len(parts))), f'PRI resource/scope collision: {locale}: {key}'
            assert all(existing.get(k)==v for k,v in data.items()), f'Stale translations: {locale}'
        else:
            raw=re.sub(r'\s*<data name="(?:EffectParam_OKLabImageAdjustment_|OKLab_|OKLabUI_)[^\"]+".*?</data>','',raw,flags=re.S)
            from xml.sax.saxutils import escape
            new='\n'.join(f'  <data name="{k}" xml:space="preserve"><value>{escape(v)}</value></data>' for k,v in data.items())
            path.write_text(raw.replace('</root>',new+'\n</root>'),encoding='utf-8-sig')
    print(f'{len(PARAMS)} OKLab parameter contracts and resources checked' if args.check else f'Generated {len(PARAMS)} OKLab parameters')


if __name__=='__main__':
    main()
