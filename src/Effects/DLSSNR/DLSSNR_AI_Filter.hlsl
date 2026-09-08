// Experimental NVIDIA DLSS neural same-resolution filter. The native D3D12
// backend replaces this pass and supplies explicit zero motion/depth guides.

//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME DLSSNR AI Filter (Experimental)

//!PARAMETER
//!GROUP HDR Protocol
//!LABEL HDR Processing Path
//!DEFAULT 0
//!OPTION 0 SDR RGBA8 compatibility
//!OPTION 1 Experimental FP16 value-domain path
int experimentalHdrPath;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Adjust Input Resolution\n(Reduces DLSSNR Quality)
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int enableInputResolutionScaling;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Input Resolution (%)
//!DEFAULT 100
//!MIN 25
//!MAX 100
//!STEP 1
int inputResolutionPercent;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Residual Multiplier
//!DEFAULT 1
//!MIN 1
//!MAX 2
//!STEP 0.05
float residualMultiplier;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Residual Saturation Multiplier
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualSaturation;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Residual Lightness Multiplier
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualLightness;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Shadow / Structure Control
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float shadowStructureMultiplier;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Reflection / Glow Control
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float reflectionGlowMultiplier;

//!PARAMETER
//!GROUP Detail Control
//!LABEL OF Quality
//!DEFAULT 2
//!OPTION 0 None
//!OPTION 1 Performance
//!OPTION 2 Balanced (Recommended)
//!OPTION 3 Quality
//!OPTION 4 High Quality (High Cost)
//!OPTION 5 Highest Quality (Very High Cost)
int motionVectorQuality;

//!PARAMETER
//!GROUP DLSSNR
//!LABEL NR Style\n(0 Default, 1 Natural, 2 Cinematic)
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 1
int style;

//!PARAMETER
//!GROUP DLSSNR
//!LABEL NR Intensity
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float intensity;

//!PARAMETER
//!GROUP DLSSNR
//!LABEL Local Tone Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float localToneStrength;

//!PARAMETER
//!GROUP DLSSNR
//!LABEL Local Structure Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float localStructureStrength;

//!PARAMETER
//!GROUP DLSSNR
//!LABEL Skin Structure Strength
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float skinStructureStrength;

//!PARAMETER
//!GROUP DLSSNR
//!LABEL Automatic Mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int useAutoMask;

//!PARAMETER
//!GROUP DLSSNR
//!LABEL NR UI Correction
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int uiCorrection;

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

MF4 Pass1(float2 pos) {
	return INPUT.SampleLevel(sam, pos, 0);
}
