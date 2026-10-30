R"BGI(
// 004AB5C0 rounded multiply/add/subtract network. Never fuse operations.
void bgi_inverse(BG_LANE) {
    BG_PRECISE float sum26=x[6]+x[2],sum04=x[4]+x[0];
    BG_PRECISE float delta26=x[2]-x[6];
    BG_PRECISE float scaled26=delta26*BG_FLOAT_BITS(0x3fb504f3u);
    BG_PRECISE float diff26=scaled26-sum26,diff04=x[0]-x[4];
    BG_PRECISE float even0=sum26+sum04,even3=sum04-sum26;
    BG_PRECISE float even1=diff26+diff04,even2=diff04-diff26;
    BG_PRECISE float diff53=x[5]-x[3],sum53=x[5]+x[3];
    BG_PRECISE float sum71=x[7]+x[1],diff17=x[1]-x[7];
    BG_PRECISE float oddtotal=sum71+sum53,oddsum=diff17+diff53;
    BG_PRECISE float z5=oddsum*BG_FLOAT_BITS(0x3fec835eu);
    BG_PRECISE float product53=BG_FLOAT_BITS(0xc0273d75u)*diff53;
    BG_PRECISE float oddfirst=product53+z5,oddB=oddfirst-oddtotal;
    BG_PRECISE float odddelta=sum71-sum53;
    BG_PRECISE float oddscaled=odddelta*BG_FLOAT_BITS(0x3fb504f3u);
    BG_PRECISE float oddC=oddscaled-oddB;
    BG_PRECISE float product17=diff17*BG_FLOAT_BITS(0x3f8a8bd4u);
    BG_PRECISE float oddlast=product17-z5,oddD=oddC+oddlast;
    x[0]=oddtotal+even0;x[1]=oddB+even1;x[2]=oddC+even2;x[3]=even3-oddD;
    x[4]=oddD+even3;x[5]=even2-oddC;x[6]=even1-oddB;x[7]=even0-oddtotal;
}
)BGI"
