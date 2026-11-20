// Record the production draw commands. Use a baked ImGui font to exercise real glyph widths and wrapping.
#include <cfloat>
#include <string>
#include <vector>
namespace NoticeUI {
struct Tile { ImVec2 a,b; ImU32 tl,tr,br,bl; };
struct Label { ImVec2 at,size; ImU32 color; std::string text; };
struct TipDraw {
    std::vector<Tile> tiles;
    std::vector<Label> labels;
    void AddRectFilledMultiColor(ImVec2 a,ImVec2 b,ImU32 tl,ImU32 tr,ImU32 br,ImU32 bl) {
        tiles.push_back({a,b,tl,tr,br,bl});
    }
    void AddText(ImFont *font,float size,ImVec2 p,ImU32 color,const char *start,const char *end=nullptr) {
        if(!end) end=start+std::strlen(start);
        labels.push_back({p,font->CalcTextSizeA(size,FLT_MAX,0,start,end),color,std::string(start,end)});
    }
} tip;
TipDraw *GetBackgroundDrawList() { return &tip; }
ImFont *GetFont() {
    static ImFontAtlas atlas;
    if(!atlas.IsBuilt()) {
        atlas.AddFontDefault();
        unsigned char *pixels; int w,h;
        atlas.GetTexDataAsAlpha8(&pixels,&w,&h);
    }
    return atlas.Fonts[0];
}
unsigned opacity(ImU32 color) { return color>>IM_COL32_A_SHIFT; }
void check_tip(const char *message,float alpha,ImVec2 display,float scale,bool wraps) {
    const auto &tiles=tip.tiles;
    const auto &labels=tip.labels;
    if(!alpha && tiles.empty()) { CHECK(labels.empty()); return; }
    CHECK(tiles.size()>3);
    const Tile &left=tiles[0], &middle=tiles[1], &right=tiles[2];
    CHECK(left.a.x>=0 && right.b.x<=display.x && left.a.y>=0);
    CHECK(std::fabs(left.a.x+right.b.x-display.x)<=2 && std::fabs(left.b.y-display.y*5/6)<=1);
    CHECK(left.b.x==middle.a.x && middle.b.x==right.a.x);
    CHECK(!opacity(left.tl) && !opacity(right.tr));
    CHECK(opacity(left.tr)==opacity(middle.tl) && opacity(middle.tr)==opacity(right.tl));
    CHECK((middle.tl&0xff)>(middle.bl&0xff) && opacity(middle.bl)>=opacity(middle.tl));
    CHECK(std::fabs(opacity(middle.tl)-185*alpha)<=1);

    // At the middle of the plate adjacent square centres alternate in both directions.
    float cell=0;
    for(size_t i=3;i<tiles.size();i++) {
        const Tile &t=tiles[i];
        CHECK(t.a.x>=left.a.x && t.b.x<=right.b.x && t.a.y>=left.a.y && t.b.y<=left.b.y);
        cell=std::fmax(cell,t.b.y-t.a.y);
    }
    CHECK(std::fabs(cell-13.2f*scale)<.02f);
    auto lit=[&](float x,float y) {
        for(size_t i=3;i<tiles.size();i++)
            if(tiles[i].a.x<=x && x<tiles[i].b.x && tiles[i].a.y<=y && y<tiles[i].b.y) return true;
        return false;
    };
    bool checked=false;
    for(size_t i=3;i<tiles.size() && !checked;i++) {
        const Tile &t=tiles[i];
        if(t.a.x<middle.a.x || t.b.x+cell>middle.b.x || t.b.y+cell>left.b.y ||
           std::fabs(t.b.x-t.a.x-cell)>.02f || std::fabs(t.b.y-t.a.y-cell)>.02f) continue;
        CHECK(std::fabs(opacity(t.tl)-17*alpha)<=1 && (t.tl&0xff)>(middle.tl&0xff));
        CHECK(!opacity(tiles[3].tl) && opacity(tiles[3].tr)<opacity(t.tl));
        for(float across : {.2f,.8f}) for(float down : {.2f,.8f}) {
            float x=t.a.x+cell*across,y=t.a.y+cell*down;
            CHECK(lit(x,y) && !lit(x+cell,y) && !lit(x,y+cell) && lit(x+cell,y+cell));
        }
        checked=true;
    }
    CHECK(checked);
    CHECK(labels.size()>=3 && labels.size()%3==0 && (labels.size()>3)==wraps);
    float stroke=std::fmax(1.f,std::round(scale));
    std::string shown;
    for(size_t i=0;i<labels.size();i+=3) {
        const auto &shadow=labels[i], &edge=labels[i+1], &label=labels[i+2];
        CHECK(shadow.text==label.text && edge.text==label.text);
        CHECK((shadow.color&0xffffff)==0 && (label.color&0xffffff)==0xffffff && (edge.color&0xffffff)==0xffffff);
        CHECK(shadow.at.x==label.at.x+stroke && shadow.at.y==label.at.y+stroke);
        CHECK(edge.at.x==shadow.at.x && edge.at.y==label.at.y);
        CHECK(std::fabs(opacity(shadow.color)-240*alpha)<=1 && std::fabs(opacity(edge.color)-170*alpha)<=1);
        CHECK(std::fabs(opacity(label.color)-255*alpha)<=1);
        CHECK(label.at.x>=left.a.x && shadow.at.x+shadow.size.x<=right.b.x+1);
        CHECK(label.at.y>=left.a.y && shadow.at.y+shadow.size.y<=left.b.y+1);
        CHECK(std::fabs(2*label.at.x+label.size.x+stroke-display.x)<=2);
        if(!shown.empty()) shown+=' ';
        shown+=label.text;
    }
    CHECK(shown==message);
}
}
