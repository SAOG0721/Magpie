#!/usr/bin/env python3
"""Magpie repo consistency checks (stdlib only, Python 3.11+).

Runs declarative consistency rules over the repository. These checks encode
rules already documented by the maintainers (docs/experimental/RELEASE-WORKFLOW.md,
docs/EXPERIMENTAL_HANDOFF_ZH.md, effect metadata conventions) so that release
preparation catches mechanical drift before a release or PR.

Usage:
    python tests/consistency/run.py [--json]

Exit code is 1 when any rule has a FAIL finding, else 0.
Rules report three levels:
  FAIL  — breaks a documented rule; must be fixed (or the rule adjusted) before merge/release.
  WARN  — suspicious; maintainer decision required (e.g. files owned by Weblate or the
          release process are reported but never auto-edited).
  INFO  — informational counters (e.g. translation coverage per language).
"""
from __future__ import annotations

import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]


class Findings:
    def __init__(self):
        self.items: list[tuple[str, str, str]] = []  # (rule, level, message)

    def add(self, rule: str, level: str, message: str) -> None:
        self.items.append((rule, level, message))

    def has_fail(self) -> bool:
        return any(level == 'FAIL' for _, level, _ in self.items)


def read_text(p: Path) -> str:
    return p.read_text(encoding='utf-8', errors='replace')


# R1: the download entry points agree with the public release manifest.
def r1_public_release(f: Findings) -> None:
    vj = json.loads(read_text(ROOT / 'version.json'))
    version, tag = vj['version'], vj['tag']
    release = f'https://github.com/SAOG0721/Magpie/releases/tag/{tag}'
    if not re.fullmatch(r'\d+\.\d+\.\d+', version) or not re.fullmatch(
        rf'v{re.escape(version)}(?:-[A-Za-z0-9.-]+)?', tag
    ) or vj['releasePage'] != release:
        f.add('R1', 'FAIL', 'version.json 的公开版本、标签或链接不一致')
        return

    for name, label in (('README.md', 'Current release:'), ('README_ZH.md', '当前版本：')):
        text = read_text(ROOT / name)
        header = re.search(rf'^\*\*{re.escape(label)}[^\n]+', text, re.MULTILINE)
        if not header or f']({release})' not in header.group():
            f.add('R1', 'FAIL', f'{name} 的当前下载链接应指向 {tag}')

    notes = read_text(ROOT / 'docs' / 'RELEASE_NOTES_NEXT.md')
    regions = re.split(r'^## 当前公开版本 / Current public release\s*$', notes, maxsplit=1, flags=re.MULTILINE)
    if len(regions) != 2 or f']({release})' not in regions[1]:
        f.add('R1', 'FAIL', f'RELEASE_NOTES_NEXT.md 的当前公开版本应指向 {tag}')
    next_section = regions[0]
    references_public_notes = f'(RELEASE_NOTES_{tag}.md)' in next_section
    if references_public_notes and re.search('草稿|未发布|尚未公开|not (?:been )?(?:uploaded|published)|draft', next_section, re.I):
        f.add('R1', 'FAIL', f'RELEASE_NOTES_NEXT.md 仍将公开版本 {tag} 列为待发布版本')
    f.add('R1', 'INFO', f'公开版本清单：{tag}')


# R2: main experimental release notes are bilingual. Accepted conventions:
#   a) Chinese block + '---' separator + English block (current standard), or
#   b) '## 中文' and '## English' (older files), or '## English quick guide'.
# Files explicitly marked as never-published drafts only WARN.
def r2_release_notes_bilingual(f: Findings) -> None:
    for p in sorted((ROOT / 'docs').glob('RELEASE_NOTES_v*-experimental.md')):
        txt = read_text(p)
        has_zh = len(re.findall(r'[一-鿿]', txt)) > 200
        has_en = ('## English' in txt or '## English quick guide' in txt
                  or txt.count('# Magpie Experimental') >= 2)
        if has_zh and has_en:
            continue
        is_draft = ('未发布' in txt or 'never officially released' in txt.lower())
        level = 'WARN' if is_draft else 'FAIL'
        f.add('R2', level, f'{p.name} 双语结构不完整(zh={has_zh}, en={has_en})')


# R3: presets reference existing effect files, and preset parameter keys must
# match the parameter variable names declared in the effect metadata.
# Metadata blocks: //!PARAMETER, then any //!-directive lines (//!GROUP,
# //!LABEL, //!DEFAULT, //!OPTION, ...), then the variable declaration.
_PARAM_RE = re.compile(r'//!PARAMETER\s*\n(?://![^\n]*\n)+(\w+) (\w+)\s*;')


def _effect_params(effect_file: Path) -> set[str]:
    txt = read_text(effect_file)
    return {m.group(2) for m in _PARAM_RE.finditer(txt)}


# Keys that ScalingModesService (V065 normalization, ScalingModesService.cpp)
# migrates or removes at load time. Preset files still carrying them are not
# silently ignored, so they only WARN; keys absent from both metadata and this
# table are silently dropped by the runtime and stay FAIL.
_R3_MIGRATED = {
    'useMotionVectors', 'useEstimatedDepth',  # DLSSFG/DLSSNR/DLSS-SR 光流选择迁移
    'motionVectorQuality',                    # DLSS/DLSSFG/DLSSNR 旧光流质量键
    'nrPreset', 'guidanceMode', 'depthInferenceInterval',  # DLSSNR 旧键(删除/迁移)
    'enableJitter',                           # SR 标记变体合并
}
# Startup and import both migrate these names in XeSSFGParameters.h.
_R3_EFFECT_ALIASES = {
    'XeSSFG/XeSS_FrameGeneration_x2_ZeroMV': 'XeSSFG/XeSS_FrameGeneration',
    'XeSSFG/XeSS_MultiFrameGeneration_ZeroMV': 'XeSSFG/XeSS_FrameGeneration',
}


def r3_presets(f: Findings) -> None:
    for p in sorted((ROOT / 'presets').glob('*.json')):
        data = json.loads(read_text(p))
        for mode in data.get('scalingModes', []):
            for eff in mode.get('effects', []):
                name = eff['name'].replace('\\', '/')
                canonical = _R3_EFFECT_ALIASES.get(name, name)
                cand = ROOT / 'src' / 'Effects' / (canonical + '.hlsl')
                if not cand.is_file():
                    f.add('R3', 'FAIL', f'{p.name}: 效果不存在 src/Effects/{canonical}.hlsl')
                    continue
                if canonical != name:
                    f.add('R3', 'WARN', f'{p.name}: 旧效果 {name} 由运行时迁移到 {canonical}')
                declared = _effect_params(cand)
                for key in (eff.get('parameters') or {}):
                    if key in declared:
                        continue
                    if key in _R3_MIGRATED:
                        f.add('R3', 'WARN',
                              f'{p.name}: {mode["name"]} → {eff["name"]} 的参数键 "{key}" '
                              '不在效果元数据中,但运行时归一化会迁移它(见 ScalingModesService)')
                        continue
                    f.add('R3', 'FAIL',
                          f'{p.name}: {mode["name"]} → {eff["name"]} 的参数键 "{key}" '
                          f'在效果元数据中不存在(声明了:{sorted(declared)})——该键会被静默忽略')


# R4: resw structural integrity + per-language coverage.
def r4_resw(f: Findings) -> None:
    langs: dict[str, set[str]] = {}
    for p in sorted((ROOT / 'src' / 'Magpie').glob('Resources.language-*.resw')):
        lang = p.stem.split('language-')[1]
        try:
            tree = ET.parse(p)
        except ET.ParseError as e:
            f.add('R4', 'FAIL', f'{p.name}: XML 解析失败 {e}')
            continue
        names = [d.attrib['name'] for d in tree.getroot().iter('data')]
        dup = {n for n in names if names.count(n) > 1}
        if dup:
            f.add('R4', 'FAIL', f'{p.name}: 重复 data 键 {sorted(dup)}')
        langs[lang] = set(names)
    ref = langs.get('en-US')
    if ref is None:
        f.add('R4', 'FAIL', '缺少 Resources.language-en-US.resw')
        return
    for lang, keys in sorted(langs.items()):
        missing = len(ref - keys)
        level = 'INFO' if lang not in ('zh-Hans', 'zh-Hant') else ('WARN' if missing else 'INFO')
        f.add('R4', level, f'{lang}: 缺少 {missing}/{len(ref)} 个键'
              + ('（翻译走 Weblate，勿在 PR 中补译文）' if missing else ''))


# R5: effect parameter label keys (PR #16 convention) consistency.
def r5_effect_param_labels(f: Findings) -> None:
    effects: dict[str, list[str]] = {}
    for p in sorted((ROOT / 'src' / 'Effects').rglob('*.hlsl')):
        txt = read_text(p)
        params = [m.group(2) for m in _PARAM_RE.finditer(txt)]
        if params:
            rel = p.relative_to(ROOT / 'src' / 'Effects').with_suffix('').as_posix()
            effects[rel] = params
    if not effects:
        f.add('R5', 'WARN', '未解析到任何效果参数(检查正则是否与 !PARAMETER 元数据格式匹配)')
        return
    en = read_text(ROOT / 'src' / 'Magpie' / 'Resources.language-en-US.resw')
    total = sum(len(v) for v in effects.values())
    have = 0
    for rel, params in effects.items():
        for param in params:
            key = 'ScalingModes_EffectParam_' + rel.replace('/', '_') + '_' + param
            if key in en:
                have += 1
    f.add('R5', 'INFO',
          f'效果参数 {total} 个;en-US 已有本地化键 {have} 个(PR#16 合入前为 0 属预期)')


# R6: resource key usage — references without definitions are FAIL;
# definitions without references are INFO (candidate cleanup; resw files are
# Weblate-managed, so removal is a maintainer decision).
def r6_resource_usage(f: Findings) -> None:
    tree = ET.parse(ROOT / 'src' / 'Magpie' / 'Resources.language-en-US.resw')
    names = [d.attrib['name'] for d in tree.getroot().iter('data')]

    def stem_of(name: str) -> str:
        # RESW contains dotted XAML property identifiers and PRI-style paths.
        uid, *attached = re.split(r'[./]\[using:', name, maxsplit=1)
        if attached:
            return uid
        separator = max(name.rfind('.'), name.rfind('/'))
        return name if separator < 0 else name[:separator]

    stems = {stem_of(n) for n in names}
    corpus = ''
    for p in (ROOT / 'src' / 'Magpie').rglob('*'):
        if p.suffix.lower() in ('.xaml', '.cpp', '.h', '.idl', '.cs'):
            corpus += read_text(p) + '\n'
    unreferenced = sorted(s for s in stems if s not in corpus)
    for s in unreferenced:
        f.add('R6', 'INFO', f'资源键无引用(废弃候选,Weblate/作者决策):{s}')
    # reverse direction: x:Uid referenced stems must exist in en-US
    uids = set(re.findall(r'x:Uid="([^"]+)"', corpus))
    for u in sorted(uids):
        if u not in stems:
            f.add('R6', 'FAIL', f'XAML 引用了 en-US 中不存在的键:{u}')


# R7: links within the checkout must resolve. Historical reviews and links
# into the surrounding workspace can refer to archived evidence.
def r7_doc_links(f: Findings) -> None:
    link_re = re.compile(r'\[[^\]]*\]\(<([^>]+)>\)|\[[^\]]*\]\(([^)\s]+)\)')
    for p in sorted((ROOT / 'docs').rglob('*.md')):
        is_review = '/reviews/' in p.as_posix()
        for m in link_re.finditer(read_text(p)):
            target = unquote((m.group(1) or m.group(2)).split('#', 1)[0])
            if re.match(r'^[a-z][a-z0-9+.-]*://|^/?[a-z]:[/\\]|^(//|\\\\)', target, re.I):
                continue
            destination = (p.parent / target).resolve()
            if not destination.exists():
                if not destination.is_relative_to(ROOT):
                    f.add('R7', 'WARN', f'{p.relative_to(ROOT)}: 工作区证据不可用 {target}')
                else:
                    level = 'WARN' if is_review else 'FAIL'
                    f.add('R7', level, f'{p.relative_to(ROOT)}: 死链 {target}'
                          + ('(历史评审记录,目标文件可能已被重构)' if is_review else ''))


# R8: files referenced by CI workflow must exist.
def r8_ci_refs(f: Findings) -> None:
    wf = read_text(ROOT / '.github' / 'workflows' / 'build.yml')
    for path in ('Magpie.slnx', 'scripts/publish.py'):
        if path in wf and not (ROOT / path).is_file():
            f.add('R8', 'FAIL', f'build.yml 引用不存在:{path}')


# R9: release script keeps the required runtime layout list.
def r9_release_layout(f: Findings) -> None:
    ps = read_text(ROOT / 'scripts' / 'Build-Release.ps1')
    for needle in ('"Magpie.exe"', '"resources.pri"', '"TouchHelper.exe"', '"Updater.exe"'):
        if needle not in ps:
            f.add('R9', 'FAIL', f'Build-Release.ps1 的运行时清单缺少 {needle}')


RULES = [
    ('R1', r1_public_release),
    ('R2', r2_release_notes_bilingual),
    ('R3', r3_presets),
    ('R4', r4_resw),
    ('R5', r5_effect_param_labels),
    ('R6', r6_resource_usage),
    ('R7', r7_doc_links),
    ('R8', r8_ci_refs),
    ('R9', r9_release_layout),
]


def main() -> int:
    f = Findings()
    for rule_id, fn in RULES:
        try:
            fn(f)
        except Exception as e:  # a crashing rule is itself a FAIL
            f.add(rule_id, 'FAIL', f'规则执行异常:{type(e).__name__}: {e}')
    for rule, level, msg in f.items:
        print(f'[{level:4}] {rule}: {msg}')
    n_fail = sum(1 for _, lv, _ in f.items if lv == 'FAIL')
    n_warn = sum(1 for _, lv, _ in f.items if lv == 'WARN')
    n_info = sum(1 for _, lv, _ in f.items if lv == 'INFO')
    print(f'\nFAIL={n_fail} WARN={n_warn} INFO={n_info}')
    if '--json' in sys.argv:
        out = {'fail': n_fail, 'warn': n_warn, 'info': n_info,
               'items': [{'rule': r, 'level': lv, 'message': m} for r, lv, m in f.items]}
        Path(sys.argv[-1] if sys.argv[-1].endswith('.json') else
             'consistency-report.json').write_text(json.dumps(out, ensure_ascii=False, indent=2),
                                                   encoding='utf-8')
    return 1 if f.has_fail() else 0


if __name__ == '__main__':
    sys.exit(main())
