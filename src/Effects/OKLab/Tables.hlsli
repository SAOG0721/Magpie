// Generated parameter gathers. Algorithms are in Adjust.hlsli.
float OKUserCurve(float x,int curve) {
    if(curve==0) {
        if(curve0Enabled==0) return x;
        float xs[8]={curve0x0,curve0x1,curve0x2,curve0x3,curve0x4,curve0x5,curve0x6,curve0x7};
        float ys[8]={curve0y0,curve0y1,curve0y2,curve0y3,curve0y4,curve0y5,curve0y6,curve0y7};
        return OKCurve8(x,xs,ys,true,false);
    }
    if(curve==1) {
        if(curve1Enabled==0) return 0;
        float xs[8]={curve1x0,curve1x1,curve1x2,curve1x3,curve1x4,curve1x5,curve1x6,curve1x7};
        float ys[8]={curve1y0,curve1y1,curve1y2,curve1y3,curve1y4,curve1y5,curve1y6,curve1y7};
        return OKCurve8(x,xs,ys,false,true);
    }
    if(curve==2) {
        if(curve2Enabled==0) return 0;
        float xs[8]={curve2x0,curve2x1,curve2x2,curve2x3,curve2x4,curve2x5,curve2x6,curve2x7};
        float ys[8]={curve2y0,curve2y1,curve2y2,curve2y3,curve2y4,curve2y5,curve2y6,curve2y7};
        return OKCurve8(x,xs,ys,false,true);
    }
    if(curve==3) {
        if(curve3Enabled==0) return 0;
        float xs[8]={curve3x0,curve3x1,curve3x2,curve3x3,curve3x4,curve3x5,curve3x6,curve3x7};
        float ys[8]={curve3y0,curve3y1,curve3y2,curve3y3,curve3y4,curve3y5,curve3y6,curve3y7};
        return OKCurve8(x,xs,ys,false,true);
    }
    if(curve==4) {
        if(curve4Enabled==0) return 0;
        float xs[8]={curve4x0,curve4x1,curve4x2,curve4x3,curve4x4,curve4x5,curve4x6,curve4x7};
        float ys[8]={curve4y0,curve4y1,curve4y2,curve4y3,curve4y4,curve4y5,curve4y6,curve4y7};
        return OKCurve8(x,xs,ys,false,false);
    }
    if(curve==5) {
        if(curve5Enabled==0) return 0;
        float xs[8]={curve5x0,curve5x1,curve5x2,curve5x3,curve5x4,curve5x5,curve5x6,curve5x7};
        float ys[8]={curve5y0,curve5y1,curve5y2,curve5y3,curve5y4,curve5y5,curve5y6,curve5y7};
        return OKCurve8(x,xs,ys,false,false);
    }
    return 0;
}
float4 OKMixer(float3 original) {
    float hs[8]={mix0Hue,mix1Hue,mix2Hue,mix3Hue,mix4Hue,mix5Hue,mix6Hue,mix7Hue};
    float cs[8]={mix0Chroma,mix1Chroma,mix2Chroma,mix3Chroma,mix4Chroma,mix5Chroma,mix6Chroma,mix7Chroma};
    float ls[8]={mix0Light,mix1Light,mix2Light,mix3Light,mix4Light,mix5Light,mix6Light,mix7Light};
    float bs[8]={bw0,bw1,bw2,bw3,bw4,bw5,bw6,bw7};
    return OKMixBands(original,hs,cs,ls,bs);
}
