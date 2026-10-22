// Experimental NVIDIA DLSS neural same-resolution filter. The native D3D12
// backend replaces this pass and uses shared optical flow with zero depth.

//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME DLSSNR AI Filter (Experimental)

//!PARAMETER
//!GROUP Detail Control
//!LABEL Adjust Input Resolution
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
//!LABEL Overall Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualMultiplier;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Chroma Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualSaturation;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Overall Lightness Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualLightness;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Shadow / Structure Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float shadowStructureMultiplier;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Highlight / Glow Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float reflectionGlowMultiplier;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Advanced Adjustments
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int residualShowAdvanced;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Optical Flow Method
//!DEFAULT 0
//!OPTION 0 None
//!OPTION 1 AMDOF
//!OPTION 2 NVOF
int opticalFlowMethod;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Optical Flow Quality
//!DEFAULT 1
//!OPTION 0 Performance
//!OPTION 1 Quality
int amdOpticalFlowMode;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Optical Flow Quality
//!DEFAULT 2
//!OPTION 1 Performance
//!OPTION 2 Balanced
//!OPTION 3 Quality
//!OPTION 4 High Quality (High Cost)
//!OPTION 5 Highest Quality (Very High Cost)
int nvidiaOpticalFlowQuality;

//!PARAMETER
//!GROUP Advanced Adjustments
//!LABEL Hue Protection
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualHueProtection;

//!PARAMETER
//!GROUP Advanced Adjustments
//!LABEL Shadow Protection
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualDarkProtection;

//!PARAMETER
//!GROUP Advanced Adjustments
//!LABEL Highlight Protection
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualHighlightProtection;

//!PARAMETER
//!GROUP Advanced Adjustments
//!LABEL Overcorrection Suppression
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualLocalCompression;

//!PARAMETER
//!GROUP Advanced Adjustments
//!LABEL Low-frequency Range Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualLowFrequencyGain;

//!PARAMETER
//!GROUP Advanced Adjustments
//!LABEL High-frequency Range Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualDetailGain;

//!PARAMETER
//!GROUP Advanced Adjustments
//!LABEL Diagnostic View
//!DEFAULT 0
//!OPTION 0 Final Image
//!OPTION 1 Raw Total Residual
//!OPTION 2 Controlled Total Residual
//!OPTION 3 Lightness Change
//!OPTION 4 Chroma Change
//!OPTION 5 Protection Weight
//!OPTION 6 Gamut Scale
//!OPTION 7 Compression Scale
int residualDebugView;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL NR Style
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 1
int style;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL NR Intensity
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float intensity;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL Local Tone Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float localToneStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL Local Structure Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float localStructureStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL Skin Structure Strength
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 0.05
float skinStructureStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL Automatic Mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int useAutoMask;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL NR UI Correction
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int uiCorrection;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL Multi Pass
//!DEFAULT 1
//!OPTION 1 1
//!OPTION 2 2
//!OPTION 3 3
int multiPass;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL Anti-flicker
//!DEFAULT 0
//!OPTION 0 None
//!OPTION 1 Static Accumulation
//!OPTION 2 Optical Flow Accumulation
//!OPTION 3 Optical Flow Accumulation+
//!OPTION 4 Low-frequency Temporal Reconstruction
int antiFlicker;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL Anti-flicker Response Time (ms)
//!DEFAULT 80
//!MIN 5
//!MAX 200
//!STEP 5
float antiFlickerTimeMs;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL NR Style
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 1
int pass2_style;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL NR Intensity
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass2_intensity;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL Local Tone Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass2_localToneStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL Local Structure Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass2_localStructureStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL Skin Structure Strength
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass2_skinStructureStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL Automatic Mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int pass2_useAutoMask;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL NR UI Correction
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int pass2_uiCorrection;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL NR Style
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 1
int pass3_style;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL NR Intensity
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass3_intensity;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL Local Tone Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass3_localToneStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL Local Structure Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass3_localStructureStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL Skin Structure Strength
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 0.05
float pass3_skinStructureStrength;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL Automatic Mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int pass3_useAutoMask;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL NR UI Correction
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int pass3_uiCorrection;

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
