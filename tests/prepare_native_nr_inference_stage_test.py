"""Extract the real inference wrappers; SDK work alone is doubled by the host."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
source = (repo / 'src/Magpie.Core/DLSSNRFilter.cpp').read_text(encoding='utf-8-sig')


def function(marker):
    start = source.index(marker)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


context_start = source.index('struct NrResourceReleaseContext {')
context_end = source.index('bool NrSynchronousReleaseRequested(')
(output / 'NativeNrInferenceWrappers.inl').write_text(
    'class NrResourceReleaseBatch;\n' + source[context_start:context_end] + '\n' +
    function('NVSDK_NGX_Result CallEvaluateFeatureSafely(') + '\n\n' +
    function('static bool SetEvaluateParametersSafely(') + '\n', encoding='utf-8')
probe = function('void NVSDK_CONV NrResourceReleaseCallback(')
probe_end = source.index(probe) + len(probe)
owner_start = source.index('struct NrReleaseInfo {')
owner = source[owner_start:probe_end]
# Observe the real Drain ticket while its real mutex is held. This only adds
# a scheduling observation in the test copy; product code/API has no hook.
ticket = '\t\tconst auto target = _statistics.accepted;'
assert owner.count(ticket) == 1
owner = owner.replace(ticket, ticket + '\n#ifdef MAGPIE_NR_TEST_DRAIN_TICKET\n'
    'MAGPIE_NR_TEST_DRAIN_TICKET(target);\n#endif')
(output / 'NativeNrResourceReleaseProbe.inl').write_text(owner + '\n', encoding='utf-8')
(output / 'NativeNrParametersWrapper.inl').write_text(
    function('static bool SetEvaluateParametersSafely(') + '\n', encoding='utf-8')
