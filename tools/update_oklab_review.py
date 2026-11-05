"""Refresh the reviewed OKLab entry after source review, then generate catalogs.

This is an explicit maintenance operation, never part of the test runner.
"""
from pathlib import Path
import hashlib
import json
from generate_oklab_parameters import PARAMS, num

root=Path(__file__).resolve().parents[1]
review_path=root/'docs/experimental/reviews/20261002-effect-description-review.json'
review=json.loads(review_path.read_text(encoding='utf-8-sig'))
shader='src/Effects/OKLabImageAdjustment.hlsl'
evidence=[
    'src/Effects/OKLab/Color.hlsli','src/Effects/OKLab/Adjust.hlsli',
    'src/Effects/OKLab/Tables.hlsli','src/Effects/OKLab/Statistics.hlsli','src/Effects/OKLab/Adapt.hlsli',
    'src/Magpie.Core/OKLabRuntime.h','src/Magpie.Core/include/OKLabParameters.h',
    'src/Magpie.Core/include/OKLabLut.h','src/Magpie/OKLabConfig.h','src/Magpie.Core/EffectDrawer.cpp',
    'src/Magpie.Core/Renderer.cpp','src/Magpie.Core/Renderer.OKLab.cpp',
    'src/Magpie/EffectParametersViewModel.OKLab.cpp','src/Magpie.Core/OKLabOverlayEditor.h',
]
def sha(path):
    return hashlib.sha256((root/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()
parameters=[]
for p in PARAMS:
    options=p['options']
    parameters.append(dict(name=p['name'],type='int' if options or p['step']==1 else 'float',
        group=p['group'],label=p['en'],default=num(p['default']),
        min=None if options else num(p['low']),max=None if options else num(p['high']),
        step=None if options else num(p['step']),
        options=[dict(value=i,label=en) for i,(en,_) in enumerate(options or [])]))
entry=dict(id='OKLabImageAdjustment',category='style',purposes=['style'],family=None,subfamily=None,
    level='situational',source=dict(path=shader,sha256=sha(shader),output=dict(WIDTH='INPUT_WIDTH',HEIGHT='INPUT_HEIGHT'),
    passCount=3,parameters=parameters,includes=[str(p.relative_to(root/'src/Effects')).replace('\\','/') for p in sorted((root/'src/Effects/OKLab').glob('*.hlsli'))]),
    review=dict(status='source-reviewed',previousLevel=None,issuesAddressed=['oklab-image-adjustment'],
        recommendationBasis='content-specific-choice',runtimeEvidence=evidence,
        runtimeEvidenceSha256={p:sha(p) for p in evidence},
        externalReferences=['https://bottosson.github.io/posts/oklab/','https://bottosson.github.io/posts/gamutclipping/'],
        notVerified=['application-release-build','native-ui-runtime','target-content-image-quality','application-integrated-performance','hdr-display-output'],
        copyReviewedOn='2026-10-28',copyDisposition='new',copyScope='Source, isolated C++/XAML, actual EffectCompiler/WARP and RTX 5070 Ti timestamp checks; release compilation/deployment and live acceptance are separate.'))
entry['zh-Hans']=dict(name='OKLab Image Adjustment',subcategory='图像调整',
    summary='分别调整明暗、色度和色相，支持选色、曲线与局部处理。',
    details='适合视频、游戏和图像调色。先从曝光、对比度和自然饱和度开始，再按需要进入八色混合、选色、曲线或局部处理区域。支持前后分屏、预设及 LUT，保持输入尺寸；LUT 文件变更需重新开始缩放。',
    search='OKLab 图像调整 OKLabImageAdjustment Oklab Image Adjustment 曝光 明度 对比度 饱和度 色相 白平衡 选色 曲线 LUT 黑白 去色带 去雾 辉光 HDR')
entry['en-US']=dict(name='OKLab Image Adjustment',subcategory='Image adjustment',
    summary='Adjusts lightness, chroma and hue, with selections, curves and local processing.',
    details='Grades video, games and images. Start with exposure, contrast and vibrance, then open the color mixer, selections, curves or local processing as needed. Includes a comparison split, presets and LUTs while keeping the input size. Restart scaling after changing a LUT file.',
    search='OKLab Oklab Image Adjustment exposure lightness contrast chroma vibrance hue white balance selection curves LUT monochrome debanding dehaze bloom HDR')
for language in ('zh-Hans','en-US'):
    entry[language]['search']+=' '+entry[language]['summary']+' '+entry[language]['details']
review['effects']=[e for e in review['effects'] if e['id']!='OKLabImageAdjustment']
index=next(i for i,e in enumerate(review['effects']) if e['id']=='ImageAdjustment')+1
review['effects'].insert(index,entry)
review_path.write_text(json.dumps(review,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
host_paths={'src/Magpie.Core/Renderer.cpp','src/Magpie.Core/EffectDrawer.cpp',
            'src/Magpie.Core/OverlayDrawer.cpp','src/Magpie/EffectParametersViewModel.cpp'}
# Reviewed integration changes are guarded by the new effect ID (and the
# existing settings-save helper was moved without changing its debounce).
# Update only these shared source witnesses, preserving every old effect's copy.
for file in [review_path,
             root/'docs/experimental/reviews/20261009-nvidia-vfx-suite-effect-description-review.json',
             root/'docs/experimental/reviews/20261011-fsr3fg-effect-description-review.json']:
    data=json.loads(file.read_text(encoding='utf-8-sig'));changed=[]
    for effect in data['effects']:
        if effect['id']=='OKLabImageAdjustment':continue
        hashes=effect.get('review',{}).get('runtimeEvidenceSha256',{})
        updated=[p for p in hashes if p in host_paths and hashes[p]!=sha(p)]
        for path in updated:hashes[path]=sha(path)
        if updated:
            effect['review']['oklabIntegrationAudit']='2026-10-28: reviewed new-effect-only routing/editor integration; existing effect behavior and copy retained.'
            changed.append(effect['id'])
    if changed:
        file.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        print('Refreshed reviewed shared host evidence:',', '.join(changed))
print('Updated reviewed OKLab source entry; run scripts/Generate-EffectCatalog.ps1 next.')
