// Experimental DLSS Frame Generation adapter. Native NGX D3D12 code replaces
// this pass and inserts generated frames before the captured real frame.

//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME DLSS FG_Experimental

//!PARAMETER
//!LABEL Frame Multiplier
//!DEFAULT 2
//!MIN 2
//!MAX 4
//!STEP 1
int multiplier;

//!PARAMETER
//!LABEL Render Preset
//!DEFAULT 0
//!OPTION 0 Auto (Default)
//!OPTION 1 A (Standard)
//!OPTION 2 B (UI Recomposition - No Effect)
//!OPTION 3 C (Performance Mode)
int renderPreset;

//!PARAMETER
//!LABEL Optical Flow Method
//!DEFAULT 0
//!OPTION 0 None
//!OPTION 1 AMDOF
//!OPTION 2 NVOF
int opticalFlowMethod;

//!PARAMETER
//!LABEL OF Quality
//!DEFAULT 1
//!OPTION 0 Performance
//!OPTION 1 Quality
int amdOpticalFlowMode;

//!PARAMETER
//!LABEL OF Quality
//!DEFAULT 2
//!OPTION 1 Performance
//!OPTION 2 Balanced (Recommended)
//!OPTION 3 Quality
//!OPTION 4 High Quality (High Cost)
//!OPTION 5 Highest Quality (Very High Cost)
int nvidiaOpticalFlowQuality;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH
//!HEIGHT INPUT_HEIGHT
Texture2D OUTPUT;

//!SAMPLER
//!FILTER POINT
SamplerState sam;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT

MF4 Pass1(float2 pos) {
	return INPUT.SampleLevel(sam, pos, 0);
}
