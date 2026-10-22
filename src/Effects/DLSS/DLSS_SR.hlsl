// Captured-colour SR adapter. Native SDK code replaces this pass.
// Depth remains zero; this is not equivalent to an in-engine integration.
//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME DLSS SR

//!PARAMETER
//!LABEL Model Preset
//!DEFAULT 11
//!OPTION 0 Auto
//!OPTION 1 A (CNN)
//!OPTION 2 B (CNN)
//!OPTION 3 C (CNN)
//!OPTION 4 D (CNN)
//!OPTION 5 E (CNN)
//!OPTION 6 F (CNN)
//!OPTION 10 J (Transformer 1)
//!OPTION 11 K (Transformer 1)
//!OPTION 12 L (Transformer 2)
//!OPTION 13 M (Transformer 2)
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
Texture2D OUTPUT;
//!SAMPLER
//!FILTER LINEAR
SamplerState sam;
//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT
MF4 Pass1(float2 pos) { return INPUT.SampleLevel(sam, pos, 0); }
