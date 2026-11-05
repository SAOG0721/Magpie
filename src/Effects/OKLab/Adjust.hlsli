// Absolute image adjustment. Masks always refer to the unadjusted source Lab.
float OKCurve8(float x,float xs[8],float ys[8],bool monotone,bool periodic) {
    xs[0]=0; xs[7]=1;
    [unroll] for(int k=1;k<7;++k) xs[k]=clamp(xs[k],xs[k-1]+.001,1-(7-k)*.001);
    if(monotone) {
        ys[0]=0; ys[7]=1;
        [unroll] for(int j=1;j<7;++j) ys[j]=clamp(ys[j],ys[j-1],1);
    }
    if(periodic) { x=frac(x); ys[7]=ys[0]; }
    // Identity tails preserve negative / HDR values for a bounded L editor.
    if(x<0 || x>1) return monotone ? x : (x<0 ? ys[0] : ys[7]);
    [unroll] for(int i=0;i<7;++i) if(x<=xs[i+1]) return lerp(ys[i],ys[i+1],(x-xs[i])/max(xs[i+1]-xs[i],.001));
    return ys[7];
}
float4 OKMixBands(float3 source,float hs[8],float cs[8],float ls[8],float bs[8]) {
    // Centers measured from sRGB red/orange/yellow/green/cyan/blue/purple/magenta.
    float centers[8]={29.2339,52.7757,109.7692,142.4953,194.7689,264.0520,293.7746,328.3634};
    float h=OKHue(source.yz), sum=0;
    float4 changes=0;
    [unroll] for(int i=0;i<8;++i) {
        float d=OKCircleDistance(h,centers[i]/360);
        float w=exp(-pow(d/(.055+.08*mixerFeather),2));
        sum+=w; changes+=w*float4(hs[i],cs[i]-1,ls[i],bs[i]);
    }
    return changes/max(sum,1e-8)*OKGrayGate(length(source.yz));
}
#include "OKLab/Tables.hlsli"
float OKToneCurve(float l) {
    if(l<=0 || l>=1) return l;
    float a=saturate(tone25),b=max(a,saturate(tone50)),c=max(b,saturate(tone75));
    return l<.25 ? l*4*a : l<.5 ? lerp(a,b,(l-.25)*4) : l<.75 ? lerp(b,c,(l-.5)*4) : lerp(c,1,(l-.75)*4);
}
float OKTone(float l) {
    l=(l-blackIn)/max(whiteIn-blackIn,.01);
    l=sign(l)*pow(abs(l),1/max(gamma,.25));
    l=(l-pivot)*contrast+pivot+lightness;
    // Ordered anchors make even extreme shadow/midtone/highlight combinations
    // monotonic. A continuous smoothstep blends each interval.
    if(shadows!=0 || midtones!=0 || highlights!=0) {
        float x1=.25+.15*balance,x2=.5+.15*balance,x3=.75+.15*balance;
        float y1=clamp(x1+.12*shadows,0,1);
        float y2=clamp(x2+.12*midtones,y1,1),y3=clamp(x3+.12*highlights,y2,1);
        if(l>0 && l<1) {
            float x0=l<x1?0:l<x2?x1:l<x3?x2:x3;
            float x4=l<x1?x1:l<x2?x2:l<x3?x3:1;
            float y0=l<x1?0:l<x2?y1:l<x3?y2:y3;
            float y4=l<x1?y1:l<x2?y2:l<x3?y3:1;
            float t=saturate((l-x0)/(x4-x0));
            // Linear endpoints retain the underlying gradation. A small smooth
            // component provides the requested feather without changing order.
            float adjustment=max(abs(shadows),max(abs(midtones),abs(highlights)));
            t=lerp(t,t*t*(3-2*t),saturate(zoneWidth/.3)*.25*adjustment);
            l=lerp(y0,y4,t);
        }
    }
    l=OKToneCurve(l);
    l=OKUserCurve(l,0);
    if(l>0 && l<1) {
        if(toe>0) l=lerp(l,l*l*(3-2*l),toe*(1-l)*.4);
        if(shoulder>0) l=lerp(l,l*l*(3-2*l),shoulder*l*.4);
    }
    return blackOut+l*(whiteOut-blackOut);
}
float OKSelect(float3 original,int enabled,float center,float width,float c0,float c1,float l0,float l1,float feather,int invert) {
    if(enabled==0) return 0;
    float c=length(original.yz);
    float w=OKHueMask(OKHue(original.yz),center,width,feather)*OKGrayGate(c);
    w*=OKBand(c,c0,c1,.1*feather)*OKBand(original.x,l0,l1,.25*feather);
    return invert!=0 ? 1-w : w;
}
float OKGeometry(float2 pos) {
    if(maskMode==0) return 1;
    float2 delta=pos-float2(maskX,maskY),dir=OKDirection(maskAngle);
    float2 p=float2(dot(delta,dir),dot(delta,float2(-dir.y,dir.x)));
    float linearWeight=saturate(p.x/maskWidth+.5);
    float w=maskMode==1 ? lerp(linearWeight,linearWeight*linearWeight*(3-2*linearWeight),maskFeather) :
        1-smoothstep(1-maskFeather,1,length(p/float2(maskWidth,maskHeight)*2));
    return maskInvert!=0 ? 1-w : w;
}
float OKSkin(float3 original) {
    float c=length(original.yz);
    return skinProtect*OKHueMask(OKHue(original.yz),skinHue,skinWidth,.5)*OKGrayGate(c)*
        OKBand(original.x,skinLightLow,skinLightHigh,.05)*OKBand(c,skinChromaLow,skinChromaHigh,.02);
}
float3 OKBilateral(float2 pos,float3 center,float radius) {
    float3 sum=center; float total=1;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        if(x==0 && y==0) continue;
        float3 lab=OKToLab(OKRead(INPUT.SampleLevel(linearSampler,pos+float2(x,y)*GetInputPt()*radius,0).rgb));
        float w=exp(-abs(lab.x-center.x)/max(edgeProtect,.005))*(x*y==0?.75:.5);
        sum+=lab*w; total+=w;
    }
    return sum/total;
}
float3 OKSpatial(float2 pos,float3 rgb,float3 original) {
    float3 lab=original;
    if(clarity!=0) lab.x+=clarity*(original.x-OKBilateral(pos,original,clarityRadius).x);
    if(textureStrength!=0) {
        float detail=original.x-OKBilateral(pos,original,textureRadius).x;
        lab.x+=textureStrength*detail*smoothstep(noiseFloor,noiseFloor*2+.0001,abs(detail));
    }
    if(chromaSmooth>0) lab.yz=lerp(lab.yz,OKBilateral(pos,original,chromaRadius).yz,chromaSmooth);
    if(deband>0) {
        float3 total=original; float weight=1;
        [unroll] for(int n=0;n<8;++n) {
            float2 dir=OKDirection(n*45+22.5);
            float3 sampleLab=OKToLab(OKRead(INPUT.SampleLevel(linearSampler,pos+dir*GetInputPt()*debandRadius,0).rgb));
            float d=length(sampleLab-original);
            float w=1-smoothstep(debandThreshold*.5,debandThreshold,d);
            total+=sampleLab*w; weight+=w;
        }
        lab+=deband*(total/weight-original);
    }
    if(clarity!=0 || textureStrength!=0 || chromaSmooth>0 || deband>0) rgb=OKFromLab(lab);
    if(bloom>0) {
        float3 glow=0;
        [unroll] for(int scale=1;scale<=3;++scale) [unroll] for(int k=0;k<8;++k) {
            float2 d=OKDirection(k*45)*bloomRadius*(float(scale)/3)*GetInputPt();
            float3 c=OKRead(INPUT.SampleLevel(linearSampler,pos+d,0).rgb);
            float y=max(OKLuma(c),0);
            float soft=smoothstep(bloomThreshold-bloomSoftness,bloomThreshold+bloomSoftness,y);
            glow+=c*soft/24;
        }
        rgb+=glow*bloom;
    }
    if(dehaze>0) {
        float3 air=max(rgb,0); float dark=1e8;
        [unroll] for(int j=0;j<8;++j) {
            float3 c=max(OKRead(INPUT.SampleLevel(linearSampler,pos+OKDirection(j*45)*dehazeRadius*GetInputPt(),0).rgb),0);
            air=max(air,c); dark=min(dark,min(c.r,min(c.g,c.b)));
        }
        air=max(air,.1);
        float t=max(.15,1-.9*dehaze*dark/max(max(air.r,air.g),air.b));
        float3 changed=OKToLab((rgb-air)/t+air), before=OKToLab(rgb);
        changed.yz=lerp(changed.yz,before.yz,dehazeProtect);
        rgb=OKFromLab(changed);
    }
    return rgb;
}
float3 OKLut(float3 rgb) {
    if(lutAmount<=0 || okLutMinimum.w<2) return rgb;
    float3 domain=lutDomain==0 ? OKEncode(rgb) : rgb;
    int edge=int(okLutMinimum.w);
    float3 normalized=(domain-okLutMinimum.xyz)/(okLutMaximum.xyz-okLutMinimum.xyz);
    float3 p=saturate(normalized)*(edge-1), base=floor(p), f=p-base;
    int3 i=min(int3(base),edge-2);
    f=p-float3(i);
    float3 result=0;
    [unroll] for(int z=0;z<2;++z) [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        int3 q=i+int3(x,y,z);
        float w=(x!=0?f.x:1-f.x)*(y!=0?f.y:1-f.y)*(z!=0?f.z:1-f.z);
        result+=Lut.Load(int3(q.x+q.z*edge,q.y,0)).rgb*w;
    }
    // A cube LUT operates on its bounded domain. Preserve out-of-range energy
    // as a residual so HDR highlights do not collapse to a cube face.
    result+=domain-clamp(domain,okLutMinimum.xyz,okLutMaximum.xyz);
    if(lutDomain==0) result=OKDecode(result);
    return lerp(rgb,result,lutAmount);
}
float4 OKAdjust(float2 pos) {
    float4 input=INPUT.SampleLevel(pointSampler,pos,0);
    if(amount==0 || (okNeutral!=0 && view==0) || (split!=0 && pos.x<splitPosition)) return input;
    float3 source=OKRead(input.rgb), original=OKToLab(source);
    float3 rgb=OKSpatial(pos,source,original);
    float4 automatic=Adaptive.Load(int3(0,0,0));
    float ev=exposure;
    if(autoExposure!=0) ev+=clamp(log2(autoTarget)-automatic.x,-autoLimit,autoLimit);
    if(autoWhite!=0) rgb*=exp2(-automatic.yzw);
    rgb*=exp2(ev);
    float3 balanced=OKToLab(OKBalance(rgb));
    float neutral=neutralProtect*(1-OKGrayGate(length(original.yz)));
    float3 lab=lerp(balanced,OKToLab(rgb),neutral);
    float tone=OKTone(lab.x);
    float protect=1-darkProtect*(1-smoothstep(0,.25,original.x));
    protect*=1-lightProtect*smoothstep(.75,1,original.x);
    lab.x=lerp(lab.x,tone,protect);
    float c=length(original.yz), h=OKHue(original.yz), skin=OKSkin(original);
    float low=1-smoothstep(.02,.12,c), high=smoothstep(.12,.25,c);
    float cg=low*chromaLow+(1-low-high)*chromaMid+high*chromaHigh;
    float3 zone=OKZones(original.x);
    cg*=dot(zone,float3(shadowChroma,midChroma,highlightChroma));
    cg*=chroma*max(0,1+vibrance*(1-smoothstep(.04,.3,c))*(1-skin));
    cg*=max(0,1+OKUserCurve(h,2)*OKGrayGate(c));
    cg*=max(0,1+OKUserCurve(original.x,4));
    cg*=max(0,1+OKUserCurve(saturate(c/.4),5));
    lab.yz=OKRotate(lab.yz*cg,hue+OKUserCurve(h,1)*180*OKGrayGate(c));
    lab.x+=OKUserCurve(h,3)*.15*OKGrayGate(c);
    float4 mixed=OKMixer(original);
    lab.yz=OKRotate(lab.yz*max(0,1+mixed.y*(1-skin)),mixed.x*(1-skin));
    lab.x+=mixed.z*(1-skin);
    float a=OKSelect(original,sel0Enabled,sel0Hue,sel0Width,sel0CLow,sel0CHigh,sel0LLow,sel0LHigh,sel0Feather,sel0Invert);
    float b=OKSelect(original,sel1Enabled,sel1Hue,sel1Width,sel1CLow,sel1CHigh,sel1LLow,sel1LHigh,sel1Feather,sel1Invert);
    float normalization=max(1,a+b), wa=a/normalization,wb=b/normalization;
    float shift=wa*sel0DeltaHue+wb*sel1DeltaHue;
    lab.yz=OKRotate(lab.yz*max(0,1+wa*(sel0Chroma-1)+wb*(sel1Chroma-1)),shift);
    lab.x+=wa*sel0Light+wb*sel1Light;
    // Vector interpolation is continuous even for opposite target hues.
    float len=length(lab.yz);
    float ua=wa*sel0Unify*OKGrayGate(c),ub=wb*sel1Unify*OKGrayGate(c);
    lab.yz=lab.yz*(1-ua-ub)+len*(ua*OKDirection(sel0TargetHue)+ub*OKDirection(sel1TargetHue));
    if(sel0Enabled!=0 || sel1Enabled!=0) lab.yz*=lerp(backgroundChroma,1,saturate(a+b));
    float2 tintVector=zone.x*wheel0Chroma*OKDirection(wheel0Hue)+zone.y*wheel1Chroma*OKDirection(wheel1Hue)+zone.z*wheel2Chroma*OKDirection(wheel2Hue);
    lab.yz+=(tintVector+float2(biasA,biasB))*(1-neutral);
    lab.x+=dot(zone,float3(wheel0Light,wheel1Light,wheel2Light));
    if(lutAmount>0 && okLutMinimum.w>=2) lab=OKToLab(OKLut(OKFromLab(lab)));
    if(styleMode==1) {
        lab.x+=mixed.w*.15*styleAmount;
        lab.yz*=1-styleAmount;
    } else if(styleMode>=2) {
        float t=saturate(lab.x);
        float2 color0=style0Chroma*OKDirection(style0Hue),color1=style1Chroma*OKDirection(style1Hue),color2=style2Chroma*OKDirection(style2Hue);
        float2 color=styleMode==2 ? lerp(color0,color2,t) : t<styleBalance ? lerp(color0,color1,t/styleBalance) : lerp(color1,color2,(t-styleBalance)/(1-styleBalance));
        lab.yz=lerp(lab.yz,color,styleAmount);
    }
    float mask=OKGeometry(pos);
    lab=lerp(original,lab,amount*mask);
    float compression; bool outside;
    rgb=OKMap(lab,compression,outside);
    float3 encoded=OKWrite(rgb);
    if(okLinearInput==0) encoded=saturate(encoded);
    if(dither>0) {
        float2 pixel=floor(pos/GetInputPt());
        float noise=frac(52.9829189*frac(dot(pixel,float2(.06711056,.00583715))))-.5;
        float levels=ditherBits==0 ? 255 : ditherBits==1 ? 1023 : 65535;
        // Dither is defined in encoded SDR, or reference-white-relative HDR units.
        encoded+=noise*dither/levels*(okLinearInput!=0?referenceWhite/80.0:1.0);
        if(okLinearInput==0) encoded=saturate(encoded);
    }
    if(view==1) encoded=saturate(lab.x).xxx;
    else if(view==2) encoded=saturate(length(lab.yz)/.4).xxx;
    else if(view==3) encoded=saturate(.5+(lab-original)*4);
    else if(view==4) encoded=outside ? float3(1,0,.5) : OKEncode(source)*.3;
    else if(view==5) encoded=compression.xxx;
    else if(view==6) encoded=a.xxx;
    else if(view==7) encoded=b.xxx;
    else if(view==8) encoded=mask.xxx;
    else if(view==9) encoded=lab.x<=0 ? float3(0,0,1) : lab.x>=pow(abs(OKLimit()),1.0/3.0) ? float3(1,0,0) : OKEncode(rgb)*.5;
    else if(view==10) encoded=OKWrite(OKFromLab(float3(lab.x,0,0)));
    if(view>=1 && view<=9 && okLinearInput!=0) encoded=OKDecode(saturate(encoded))*(referenceWhite/80.0);
    return float4(encoded,input.a);
}
