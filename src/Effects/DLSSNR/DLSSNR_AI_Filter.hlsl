// Experimental NVIDIA DLSS neural same-resolution filter. The native D3D12
// backend replaces this pass and uses shared optical flow with zero depth.

//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME DLSSNR AI Filter (Experimental)

//!PARAMETER
//!GROUP Detail Control
//!LABEL Model Preset
//!DEFAULT 0
//!OPTION 0 Automatic (DLL Default)
//!OPTION 1 HT
//!OPTION 2 CC Small FP8
int modelPreset;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Adjust Input Resolution
//!DEFAULT 1
//!MIN 0
//!MAX 1
//!STEP 1
int enableInputResolutionScaling;

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
//!LABEL Midtone Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualMidtoneStrength;

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
//!LABEL Residual
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int residualShowAdvanced;

//!PARAMETER
//!GROUP Detail Control
//!LABEL Performance
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int performanceShowAdvanced;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
//!LABEL Low-frequency Range Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualLowFrequencyGain;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
//!LABEL High-frequency Range Strength
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float residualDetailGain;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
//!LABEL Hue Protection
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualHueProtection;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
//!LABEL Chroma Protection
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualChromaProtection;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
//!LABEL Shadow Protection
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualDarkProtection;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
//!LABEL Highlight Protection
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualHighlightProtection;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
//!LABEL Overcorrection Suppression
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 0.05
float residualLocalCompression;

//!PARAMETER
//!GROUP Residual Advanced Adjustments
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
//!GROUP Performance Advanced Adjustments
//!LABEL Resolution Control
//!DEFAULT 0
//!OPTION 0 Adaptive
//!OPTION 1 Manual
int resolutionControl;

//!PARAMETER
//!GROUP Performance Advanced Adjustments
//!LABEL Input Resolution (%)
//!DEFAULT 100
//!MIN 25
//!MAX 100
//!STEP 1
int inputResolutionPercent;

//!PARAMETER
//!GROUP Performance Advanced Adjustments
//!LABEL Processing Mode
//!DEFAULT 0
//!OPTION 0 Every Frame
//!OPTION 1 Alternating Frames
int processingMode;

//!PARAMETER
//!GROUP Performance Advanced Adjustments
//!LABEL NR Update Interval
//!DEFAULT 2
//!OPTION 2 Every 2 Frames
//!OPTION 3 Every 3 Frames
//!OPTION 4 Every 4 Frames
int sparseUpdateInterval;

//!PARAMETER
//!GROUP Performance Advanced Adjustments
//!LABEL Residual Reuse
//!DEFAULT 0
//!OPTION 0 Historical Residual in Place
//!OPTION 1 Historical Residual Reprojection
int residualTransfer;

//!PARAMETER
//!GROUP Performance Advanced Adjustments
//!LABEL Optical Flow Method
//!DEFAULT 0
//!OPTION 0 None
//!OPTION 1 AMDOF
//!OPTION 2 NVOF
int opticalFlowMethod;

//!PARAMETER
//!GROUP Performance Advanced Adjustments
//!LABEL Optical Flow Quality
//!DEFAULT 1
//!OPTION 0 Performance
//!OPTION 1 Quality
int amdOpticalFlowMode;

//!PARAMETER
//!GROUP Performance Advanced Adjustments
//!LABEL Optical Flow Quality
//!DEFAULT 2
//!OPTION 1 Performance
//!OPTION 2 Balanced
//!OPTION 3 Quality
//!OPTION 4 High Quality
//!OPTION 5 Highest Quality
int nvidiaOpticalFlowQuality;

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
//!LABEL Auto mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int useAutoMask;

//!PARAMETER
//!GROUP DLSSNR · Pass 1
//!LABEL UI fix
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
//!LABEL Auto mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int pass2_useAutoMask;

//!PARAMETER
//!GROUP DLSSNR · Pass 2
//!LABEL UI fix
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
//!LABEL Auto mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int pass3_useAutoMask;

//!PARAMETER
//!GROUP DLSSNR · Pass 3
//!LABEL UI fix
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
