// Native AMDNR/lmxxf runtime replaces this descriptor. No shader fallback.
//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME AMD lmxxf NR

//!PARAMETER
//!GROUP AMD NR
//!LABEL NR Style Feature Strength
//!DEFAULT 1
//!MIN 0
//!MAX 4
//!STEP 0.05
float style;

//!PARAMETER
//!GROUP AMD NR
//!LABEL NR Intensity
//!DEFAULT 1
//!MIN 0
//!MAX 1
//!STEP 0.05
float intensity;

//!PARAMETER
//!GROUP AMD NR
//!LABEL Local Tone Strength
//!DEFAULT 1
//!MIN 0
//!MAX 4
//!STEP 0.05
float localToneStrength;

//!PARAMETER
//!GROUP AMD NR
//!LABEL Local Structure Strength
//!DEFAULT 1
//!MIN 0
//!MAX 4
//!STEP 0.05
float localStructureStrength;

//!PARAMETER
//!GROUP AMD NR
//!LABEL Skin Structure Strength
//!DEFAULT 1
//!MIN 0
//!MAX 4
//!STEP 0.05
float skinStructureStrength;

//!PARAMETER
//!GROUP AMD NR
//!LABEL Automatic Character Mask
//!DEFAULT 1
//!OPTION 0 Off
//!OPTION 1 On
int useAutoMask;

//!PARAMETER
//!GROUP AMD NR
//!LABEL Multi Pass
//!DEFAULT 1
//!OPTION 1 1
//!OPTION 2 2
//!OPTION 3 3
int multiPass;

//!TEXTURE
Texture2D INPUT;
//!TEXTURE
//!WIDTH INPUT_WIDTH
//!HEIGHT INPUT_HEIGHT
Texture2D OUTPUT;
//!SAMPLER
//!FILTER LINEAR
SamplerState sam;
//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT
MF4 Pass1(float2 pos) { return INPUT.SampleLevel(sam, pos, 0); }
