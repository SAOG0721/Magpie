float4 OKAdapt() {
    float4 current=0;
    [loop] for(int y=0;y<9;++y) [loop] for(int x=0;x<16;++x) current+=Stats.Load(int3(x,y,0));
    current/=144;
    float4 previous=History.Load(int3(0,0,0));
    if(okHistoryValid==0) return current;
    if(autoLock!=0) return previous;
    // Accelerate sustained scene changes, but do not snap to a one-frame flash.
    // Both paths depend on elapsed captured-frame time, not presentation rate.
    float speed=abs(current.x-previous.x)>sceneThreshold ? min(autoSpeed,.15) : autoSpeed;
    return lerp(previous,current,1-exp(-min(okDeltaSeconds,1.0)/max(speed,.05)));
}
