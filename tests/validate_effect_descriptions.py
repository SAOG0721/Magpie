"""Catalog/resource/source contracts only; does not compile or run Magpie."""
import json
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')


def literal_ids(text):
    return [value.replace('\\\\', '\\') for value in re.findall(r'"([^"\n]+)"', text)]


def documented_ids(path, section):
    block = read(path).split(f'<!-- {section}:start -->', 1)[1].split(f'<!-- {section}:end -->', 1)[0]
    ids = re.findall(r'`([^`]+)`', block)
    if len(ids) != len(set(ids)):
        raise AssertionError(f'Duplicate documented effect: {section}')
    return set(ids)


class EffectDescriptionContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalogs = {lang: json.loads(read(f'src/Magpie/EffectCatalog/{lang}.json'))
                        for lang in ('zh-Hans', 'en-US')}
        cls.effects = {lang: {e['id']: e for e in catalog['effects']}
                       for lang, catalog in cls.catalogs.items()}
        cls.resources = {}
        for lang in cls.catalogs:
            nodes = ET.fromstring(read(f'src/Magpie/Resources.language-{lang}.resw')).findall('data')
            cls.resources[lang] = {n.attrib['name']: n.findtext('value') for n in nodes}
            if len(nodes) != len(cls.resources[lang]):
                raise AssertionError(f'Duplicate resource key: {lang}')

    def test_new_effects_have_real_filter_destinations(self):
        for lang, effects in self.effects.items():
            categories = {c['id']: c for c in self.catalogs[lang]['categories']}
            for id, category in [(r'NvidiaVFX\Upscale', 'upscale'),
                                 (r'NvidiaVFX\VFG', 'frame'),
                                 (r'FSR3FG\FSR3_FrameGeneration', 'frame')]:
                entry = effects[id]
                self.assertEqual(entry['category'], category)
                # First subcategory is General purpose / Frame generation.
                self.assertEqual(entry['subcategory'], categories[category]['subcategories'][0]['name'])

    def test_advanced_shortcut_points_to_installed_entries(self):
        block = read('src/Magpie/EffectPickerModel.h').split('ADVANCED_PICKER_EFFECTS[] = {', 1)[1].split('};', 1)[0]
        ids = literal_ids(block)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertLessEqual(set(ids), set(self.effects['en-US']))
        self.assertLessEqual({r'NvidiaVFX\Upscale', r'NvidiaVFX\VFG',
                              r'FSR3FG\FSR3_FrameGeneration'}, set(ids))

    def test_post_effect_allowlist_matches_installed_ids(self):
        source = read('src/Magpie.Core/include/NvidiaVfxExecutionPlan.h')
        body = source.split('inline bool NvidiaVfxPostEffectSupported', 1)[1].split('\n}', 1)[0]
        ids = set(literal_ids(body))
        self.assertEqual(ids, {'Bicubic', 'Bilinear', r'CAS\CAS', r'FSR\FSR_RCAS'})
        self.assertLessEqual(ids, set(self.effects['en-US']))
        # The native test exercises admission/rejection after the TrueHDR
        # boundary. This cross-file check catches misspelled installed IDs.
        native = read('tests/NvidiaVfxPolicyTests.cpp')
        for id in (r'FSR\FSR_RCAS', r'RCAS\RCAS', r'CAS\CAS_Scaling', r'CRT\CRT_Geom'):
            self.assertIn(id.replace('\\', '\\\\'), native)

    def test_full_compatibility_lists_follow_runtime_policy(self):
        # Full chains belong in usage docs. Compare their IDs with the actual
        # admission rules so concise picker copy cannot silently lose a rule.
        fsr = read('src/Magpie.Core/FSR3FGPolicy.h').split(
            'constexpr bool SupportsFSR3FGExternalMotion', 1)[1].split('\n}', 1)[0]
        fsr_ids = set(literal_ids(fsr))
        self.assertEqual(documented_ids('docs/experimental/effects/FSR3FG.md',
                                        'fsr3fg-supported-effects'), fsr_ids)
        self.assertLessEqual(fsr_ids - {r'Utility\FrameRate_Filter'}, set(self.effects['en-US']))
        vfx = read('src/Magpie.Core/include/NvidiaVfxExecutionPlan.h').split(
            'inline bool NvidiaVfxPostEffectSupported', 1)[1].split('\n}', 1)[0]
        constants = read('src/Magpie.Core/include/NvidiaVfxParameters.h')
        vfx_ids = set(literal_ids(vfx))
        for name in set(re.findall(r'\bNVIDIA_VFX_\w+_ID\b', vfx)):
            value = re.search(r'\b' + name + r'\s*=\s*"([^"]+)"', constants)
            self.assertIsNotNone(value, name)
            vfx_ids.add(value[1].replace('\\\\', '\\'))
        self.assertEqual(documented_ids('docs/Built-in effects.md', 'vfx-post-effects'), vfx_ids)
        self.assertLessEqual(vfx_ids, set(self.effects['en-US']))
        # Users can reach both usage sections from the repository's main pages.
        for path in ('README.md', 'README_ZH.md'):
            self.assertIn('docs/Built-in%20effects.md#current-branch-setup', read(path))
        self.assertIn('effects/FSR3FG.md#compatible-effect-list', read('docs/Built-in effects.md'))
        self.assertIn('Balanced/Preset J', read('docs/Built-in effects.md'))

    def test_current_names_and_legacy_search_aliases(self):
        def normalize(text):
            return re.sub(r'[_\\/\-\s]', '', text).lower()
        for lang, effects in self.effects.items():
            for id, e in effects.items():
                searchable = normalize(e['search'])
                self.assertIn(normalize(id), searchable, (lang, id))
                self.assertIn(normalize(e['name']), searchable, (lang, id))
                self.assertIn(normalize(e['summary']), searchable, (lang, id))
            for family in ('VSR', 'Denoise'):
                for tier in ('Low', 'Medium', 'High', 'Ultra'):
                    self.assertIn(f'RTXVideo\\RTXVideo_{family}_{tier}', effects[r'RTXVideo\RTXVideo_VSR']['search'])
            for alias in ('FSR3FG', 'FSR 3 FG', 'FSR3_FrameGeneration'):
                self.assertIn(alias, effects[r'FSR3FG\FSR3_FrameGeneration']['search'])
            self.assertIn('RTX Video HDR', effects[r'RTXVideo\RTXVideo_HDR']['search'])

    def test_new_parameter_help_is_bilingual_and_reachable(self):
        parameters = {
            r'NvidiaVFX\Upscale': ['sharpness'],
            r'NvidiaVFX\VFG': ['model', 'multiplier', 'automaticShotDetection'],
            r'RTXVideo\RTXVideo_VSR': ['category', 'strength'],
            r'RTXVideo\RTXVideo_HDR': ['contrast', 'saturation', 'middleGray', 'peakNits', 'debanding'],
            r'FSR3FG\FSR3_FrameGeneration': ['opticalFlowMethod', 'amdOpticalFlowMode', 'nvidiaOpticalFlowQuality'],
            r'Color\HDR_to_SDR': ['mode', 'whiteNits', 'peakNits', 'exposure', 'shoulder'],
            r'Color\SDR_to_HDR': ['mode', 'nativeWhiteNits'],
            r'DLSSFG\DLSS_FrameGeneration': ['multiplier'],
            r'XeSSFG\XeSS_FrameGeneration': ['multiplier'],
        }
        for scale in ('2x', '3x', '4x'):
            parameters['xBR\\xBR_' + scale] = ['paramXbrYWeight', 'paramXbrEqThreshold',
                                               'paramXbrLv2Coefficient', 'paramPreserveSmallDetails']
        parameters[r'xBR\xBR_NoBlend_3x'] = ['paramXbrLv2Coefficient']
        for id in (r'DLSS\DLSS_SR', r'FSR2\FSR2_SR', r'FSR3\FSR3_SR',
                   r'FSR4\FSR4_SR', r'XeSS\XeSS_SR', r'DLSSFG\DLSS_FrameGeneration',
                   r'XeSSFG\XeSS_FrameGeneration', r'DLSSNR\DLSSNR_AI_Filter'):
            parameters.setdefault(id, []).extend(
                ['opticalFlowMethod', 'amdOpticalFlowMode', 'nvidiaOpticalFlowQuality'])
        for effect, params in parameters.items():
            shader = read('src/Effects/' + effect.replace('\\', '/') + '.hlsl')
            for param in params:
                self.assertRegex(shader, r'\b(?:int|float) ' + param + ';')
                key = 'EffectParam_' + re.sub(r'[^A-Za-z0-9]', '_', effect) + '_' + param + '_Description'
                for lang, resources in self.resources.items():
                    self.assertTrue(resources.get(key), (lang, key))
        for lang, resources in self.resources.items():
            for key in ('EffectPicker_CustomSummary', 'EffectPicker_CustomDetails',
                        'EffectPicker_CustomCategoryDescription', 'NvidiaVfx_Upscale_AutoHelp',
                        'NvidiaVfx_Upscale_FixedHelp'):
                self.assertTrue(resources[key], (lang, key))
        # Both parameter surfaces must still use the common localized helper.
        for path in ('src/Magpie/EffectParametersViewModel.cpp', 'src/Magpie.Core/OverlayDrawer.cpp'):
            self.assertIn('EffectParameterLocalization::Tooltip', read(path))

    def test_essential_conditions_and_moved_parameter_help(self):
        for lang, effects in self.effects.items():
            fsr = effects[r'FSR3FG\FSR3_FrameGeneration']
            self.assertRegex(fsr['summary'], r'2[×x]')
            self.assertIn('SDR', fsr['summary'])
            for token in ('AMDOF', 'x64'):
                self.assertIn(token, fsr['details'])
            self.assertRegex(fsr['details'], r'实验性|Experimental')
            vfg = effects[r'NvidiaVFX\VFG']
            for token in ('SDR', 'Ada', 'Blackwell', 'x64'):
                self.assertIn(token, vfg['details'])
            self.assertIn('Ampere', effects[r'RTXVideo\RTXVideo_VSR']['details'])
            self.assertNotIn('重启应用', effects[r'RTXVideo\RTXVideo_VSR']['details'])
            self.assertRegex(effects[r'xBR\xBR_Hybrid_2x']['details'], r'最近邻 2×|nearest-neighbor 2x')
            self.assertIn('SDR', effects[r'xBR\xBR_NoBlend_3x']['details'])
            for id in (r'Color\HDR_to_SDR', r'Color\SDR_to_HDR', r'RTXVideo\RTXVideo_HDR'):
                self.assertIn('Windows HDR', effects[id]['details'])
            help = self.resources[lang]['EffectParam_DLSSNR_DLSSNR_AI_Filter_residualMultiplier_Description']
            self.assertRegex(help, r'全部处理层|all passes')
            self.assertTrue(self.resources[lang]['EffectParam_DLSSNR_DLSSNR_AI_Filter_modelPreset_Description'])
            for effect, param in ((r'RTXVideo\RTXVideo_VSR', 'category'),
                                  (r'FSR3FG\FSR3_FrameGeneration', 'opticalFlowMethod'),
                                  (r'DLSS\DLSS_SR', 'opticalFlowMethod')):
                key = 'EffectParam_' + re.sub(r'[^A-Za-z0-9]', '_', effect) + '_' + param + '_Description'
                self.assertRegex(self.resources[lang][key], r'重新启动缩放|Restart scaling')

    def test_dlssnr_two_mode_controls_and_localized_help(self):
        shader = read('src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl')
        modes = re.search(r'//!PARAMETER\s*\n(?:(?!//!PARAMETER).)*int processingMode;', shader, re.S)[0]
        self.assertEqual(re.findall(r'//!OPTION (\d+) (.*)', modes), [
            ('0', 'Every Frame'), ('1', 'Alternating Frames')])
        interval = re.search(r'//!PARAMETER\s*\n(?:(?!//!PARAMETER).)*int sparseUpdateInterval;', shader, re.S)[0]
        self.assertRegex(interval, r'//!DEFAULT 2\s')
        self.assertEqual(re.findall(r'//!OPTION (\d+) (.*)', interval), [
            ('2', 'Every 2 Frames'), ('3', 'Every 3 Frames'), ('4', 'Every 4 Frames')])
        retired = ('recoveryPolicy', 'bufferedDelayFrames', 'asyncResidualComposition', '__dlssnrModeVersion')
        for name in retired:
            self.assertNotRegex(shader, r'\bint ' + name + ';')
        for language in ('en-US', 'zh-Hans', 'zh-Hant'):
            nodes = ET.fromstring(read(f'src/Magpie/Resources.language-{language}.resw')).findall('data')
            resources = {node.attrib['name']: node.findtext('value') for node in nodes}
            prefix = 'EffectParam_DLSSNR_DLSSNR_AI_Filter_'
            for value in range(2):
                self.assertTrue(resources[prefix + f'processingMode_Option_{value}'])
            for value in (2, 3, 4, 5, 6):
                self.assertNotIn(prefix + f'processingMode_Option_{value}', resources)
            for name in retired:
                self.assertFalse(any(key.startswith(prefix + name + '_') for key in resources))
            for suffix in ('Label', 'Description', 'Option_2', 'Option_3', 'Option_4'):
                self.assertTrue(resources[prefix + 'sparseUpdateInterval_' + suffix])
            self.assertNotIn('K', resources[prefix + 'sparseUpdateInterval_Description'])
            self.assertNotIn(prefix + 'FixedResolutionRequired', resources)
            for key, value in resources.items():
                if key.startswith(prefix):
                    self.assertNotRegex(value, r'Async Every Frame|Async Alternating Frames|异步逐帧|异步隔帧|非同步逐幀|非同步隔幀')
            self.assertEqual(resources['FrameSync_Mode_Async.Content'], 'Async')
        english = self.effects['en-US'][r'DLSSNR\DLSSNR_AI_Filter']['details']
        for label in ('Every Frame', 'Alternating Frames'):
            self.assertIn(label, english)
        for label in ('Async Every Frame', 'Async Alternating Frames', 'Buffered Balanced', 'Sparse Economy',
                      'Bidirectional Quality', 'Recovery Policy', 'lightweight NR'):
            self.assertNotIn(label, english)


if __name__ == '__main__':
    unittest.main(verbosity=2)
