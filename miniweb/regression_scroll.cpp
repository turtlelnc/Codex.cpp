#include "src/engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>
void check_scroll(const std::filesystem::path& directory,bool report){
 Loader::set_access_root(directory.u8string());Engine e(640,480);e.set_wait_ms(0);std::string error;
 auto check=[](bool yes,const char* message){if(!yes)throw std::runtime_error(message);};
 check(e.load((directory/"scroll.html").u8string(),&error)&&e.render(false),"Scroll fixture load");auto* d=e.document();auto node=[&](const char* id){return d->get_element_by_id(id);};
 check(node("panel")->scroll_height==700&&node("panel")->scroll_width==400,"Scrollable overflow extents");
 check(e.scroll(50,150,100,260)&&e.render(false),"Scroll nested panel");
 check(node("panel")->scroll_x==100&&node("panel")->scroll_y==260&&d->body->attr("data-scroll")=="100:260","Native offset and scroll event");
 auto r=node("target")->viewport_rect(),s=node("sticky")->viewport_rect();
 check(std::abs(r.x-40)<.1&&std::abs(r.y-80)<.1&&std::abs(s.y-60)<.1,"Scrolled and sticky viewport geometry");
 auto pixel=[&](int x,int y,int red,int green,int blue){auto i=(y*640+x)*4;const auto& p=e.canvas()->pixels();check(p[i]==red&&p[i+1]==green&&p[i+2]==blue,"Scroll pixels agree with geometry");};
 pixel(45,105,0,0,255);pixel(25,65,0,128,0);pixel(305,15,255,0,0);
 check(e.click(45,105)&&d->body->attr("data-click")=="target","Scrolled target receives click");
 d->body->set_attr("data-coalesce","");check(e.click(305,15)&&node("panel")->scroll_y==30&&d->body->attr("data-scroll-count")=="2","Repeated script scroll changes coalesce into one event");d->body->remove_attr("data-coalesce");set_scroll(node("panel"),100,260);check(e.render(false),"Restore scrolled panel");
 node("panel")->set_attr("style","overflow-x:hidden;overflow-y:auto");set_scroll(node("panel"),0,0);
 check(e.render(false)&&e.scroll(50,150,50,50)&&node("panel")->scroll_x==0&&node("panel")->scroll_y==50,"Diagonal wheel respects each axis overflow policy");
 node("panel")->remove_attr("style");check(e.render(false),"Restore auto overflow");set_scroll(node("panel"),100,260);
 check(e.scroll(600,400,0,400)&&e.render(false)&&d->scroll_y==400&&d->body->attr("data-window-scroll")=="400","Page scroll events and state");
 check(std::abs(node("target")->viewport_rect().y-(r.y-400))<.1,"Document contents move with viewport scroll");
 check(node("fixed")->viewport_rect().y==10&&e.click(305,15)&&d->body->attr("data-click")=="fixed","Fixed geometry and hit testing survive viewport scroll");
 check(e.scroll(305,15,0,100)&&d->scroll_y==400&&d->body->attr("data-wheel")=="canceled","Cancelable wheel default");
 check(e.scroll(600,400,0,-10000)&&d->scroll_y==0,"Page scroll clamps at origin");
 check(set_scroll(node("hidden"),50,50)&&node("hidden")->scroll_y==50,"Hidden overflow permits programmatic scroll");
 check(!set_scroll(node("clip"),50,50)&&node("clip")->scroll_y==0,"Clip forbids scrolling");
 check(e.scroll(50,150,10000,10000)&&node("panel")->scroll_x==180&&node("panel")->scroll_y==520,"Nested offsets clamp at content bounds");
 check(e.scroll(50,150,0,100)&&d->scroll_y==100,"Scroll at container boundary chains to viewport");
 node("target")->detach();node("content")->set_attr("style","height:100px");check(e.render(false)&&node("panel")->scroll_y==0&&d->body->attr("data-scroll")=="180:0","Content shrink clamps offset and emits scroll event");
 d->html->set_attr("style","height:480px;overflow:hidden");check(e.render(false)&&e.scroll(600,400,0,100)&&d->scroll_y==100,"Hidden viewport rejects wheel scrolling");
 check(set_scroll(d->html,0,400)&&e.render(false)&&d->scroll_y==400,"Hidden viewport still permits programmatic scrolling");pixel(305,15,255,0,0);
 check(!e.scroll(-1,0,0,10)&&!e.scroll(0,0,0,10001)&&!e.scroll(0,0,NAN,10),"Invalid scroll operations rejected");
 d->body->set_attr("style","height:500px");check(e.render(false)&&d->scroll_y==20&&d->body->attr("data-window-scroll")=="20","Viewport clamp emits scroll event");
 node("panel")->set_attr("style","display:none");check(e.render(false)&&e.click(305,15)&&d->body->attr("data-panel-metrics")=="0:0:0","Hidden element exposes no stale scroll geometry");
 node("panel")->detach();check(e.render(false)&&e.click(305,15)&&d->body->attr("data-panel-metrics")=="0:0:0","Detached element exposes no stale scroll geometry");
 Engine clipped(640,480);clipped.set_wait_ms(0);check(clipped.load((directory/"scroll-clip-sticky.html").u8string(),&error)&&clipped.render(false)&&clipped.scroll(600,400,0,200)&&clipped.render(false),"Clip wrapper sticky fixture");
 check(clipped.document()->get_element_by_id("header")->viewport_rect().y==0&&clipped.click(10,10)&&clipped.document()->body->attr("data-click")=="sticky","Clip-only ancestor does not establish a sticky scrollport");
 Engine flow(640,480);flow.set_wait_ms(0);check(flow.load((directory/"scroll-sticky-flow.html").u8string(),&error)&&flow.render(false),"Late sticky fixture");
 check(flow.document()->get_element_by_id("late")->viewport_rect().y==300&&flow.click(5,80)&&flow.document()->body->attr("data-click").empty(),"Top-only sticky remains clipped and cannot be clicked before activation");
 check(flow.scroll(50,50,0,200)&&flow.document()->get_element_by_id("late")->viewport_rect().y==100,"Sticky remains below the viewport before threshold");
 check(flow.scroll(50,50,0,100)&&flow.document()->get_element_by_id("late")->viewport_rect().y==0&&flow.click(5,5)&&flow.document()->body->attr("data-click")=="late","Sticky activates at its normal-flow threshold");
 check(flow.scroll(50,50,0,150)&&flow.document()->get_element_by_id("late")->viewport_rect().y==0,"Direct scroll-container child uses scrolling-content bounds");
 Engine sidebar(640,800);sidebar.set_wait_ms(0);
 check(sidebar.load((directory/"sidebar-layout.html").u8string(),&error)&&sidebar.render(false),"Sidebar fixture");
 auto sidebar_node=[&](const char* id){return sidebar.document()->get_element_by_id(id);};
 check(sidebar_node("viewport")->viewport_rect().h==160,"Dynamic viewport units");
 check(sidebar_node("sidebar")->viewport_rect().h==300&&sidebar_node("header")->viewport_rect().h==40&&
       sidebar_node("list")->viewport_rect().h==230,
       "Definite Grid track constrains the flex scroller");
 check(sidebar_node("fractional-scroll")->viewport_rect().h==100,"Fractional Grid row retains intrinsic height");
 check(sidebar_node("auto-track")->viewport_rect().h==70,"Fixed and auto Grid tracks share definite height");
 check(sidebar_node("settings")->viewport_rect().x==100,"Flex auto margin takes remaining width");
 check(sidebar_node("content-minimum")->viewport_rect().h==200&&
       sidebar_node("content-remainder")->viewport_rect().h==100,"Column Flex preserves automatic minimum heights");
 check(sidebar_node("scroll-flex")->viewport_rect().h==150&&sidebar_node("zero-flex")->viewport_rect().h==150,
       "Scrollable and explicit zero-minimum Flex items may shrink");
 check(sidebar_node("capped-flex")->viewport_rect().h==120&&sidebar_node("capped-remainder")->viewport_rect().h==180,
       "Column Flex redistributes space after an item reaches its maximum");
 check(sidebar_node("empty-nested-flex")->viewport_rect().h==30&&
       sidebar_node("empty-nested-grid")->viewport_rect().h==30,"Empty nested containers shrink below specified height");
 check(sidebar_node("filled-nested-flex")->viewport_rect().h==35&&
       sidebar_node("filled-nested-grid")->viewport_rect().h==35,"Nested containers retain their intrinsic minimum");
 check(sidebar.scroll(20,220,0,100)&&sidebar.render(false)&&sidebar_node("list")->scroll_y==100,
       "Sidebar wheel scroll stays in the list");
 check(sidebar.document()->scroll_y==0&&sidebar_node("footer")->viewport_rect().y==430,
       "Sidebar footer stays in place while scrolling");
 Engine app(640,800);app.set_wait_ms(0);
 check(app.load((directory/"app-layout.html").u8string(),&error)&&app.render(false),"App layout fixture");
 auto app_node=[&](const char* id){return app.document()->get_element_by_id(id);};
 check(std::abs(app_node("first")->viewport_rect().h-106.667f)<.1f&&
       app_node("footer")->viewport_rect().y==376,"Fractional tracks share definite height");
 check(app_node("quarter")->viewport_rect().h==25,"Subunit fractions leave unused space");
 check(app_node("bounded-track")->viewport_rect().h==70,"Explicit Grid track minimum");
 check(app_node("top")->viewport_rect().y==490&&app_node("bottom")->viewport_rect().y==590,
       "Column auto margin takes precedence over justification");
 check(app.scroll(30,90,0,200)&&app.render(false)&&app_node("first")->scroll_y==200&&
       app_node("sticky")->viewport_rect().y==40&&app_node("footer")->viewport_rect().y==376,
       "Fractional Grid scroller keeps its sticky header and footer in place");
 Engine details(640,480);details.set_wait_ms(0);
 check(details.load((directory/"details.html").u8string(),&error)&&details.render(false),"Details fixture");
 auto node_in_details=[&](const char* id){return details.document()->get_element_by_id(id);};
 auto* disclosure=node_in_details("files");
 auto* summary=node_in_details("summary");
 auto* detail_body=details.document()->body;
 check(disclosure->viewport_rect().h==30&&!node_in_details("file")->layout.has_box&&
       !node_in_details("extra")->layout.has_box,"Closed details only lays out its first summary");
 check(details.key("Tab")&&details.document()->focused_id==summary->id&&details.key("Enter")&&
       disclosure->has_attr("open")&&details.render(false)&&node_in_details("file")->layout.has_box,
       "Tab and Enter expose details contents");
 check(details.advance(1)&&detail_body->attr("data-toggle")=="closed:open","Details dispatches toggle");
 detail_body->set_attr("data-keys","");
 detail_body->set_attr("data-cancel-keyup","");
 check(details.key(" ")&&disclosure->has_attr("open")&&detail_body->attr("data-keys")=="keydown keyup ",
       "Canceled key release leaves disclosure open");
 detail_body->remove_attr("data-cancel-keyup");
 detail_body->set_attr("data-keys","");
 check(details.key(" ")&&!disclosure->has_attr("open")&&details.render(false)&&
       detail_body->attr("data-keys")=="keydown keyup click ","Space activates disclosure after key release");
 detail_body->set_attr("data-cancel","");
 check(details.click(10,10)&&!disclosure->has_attr("open"),"Canceled summary click preserves state");
 detail_body->remove_attr("data-cancel");
 const auto button=node_in_details("embedded")->viewport_rect();
 check(details.click(button.x+5,button.y+5)&&!disclosure->has_attr("open"),"Nested button does not toggle details");
 check(details.click(10,35)&&details.render(false)&&disclosure->has_attr("open")&&
       node_in_details("footer")->viewport_rect().y>30,"Script-opened details reflows");
 auto* fallback=node_in_details("fallback");
 auto* fallback_content=node_in_details("fallback-content");
 check(fallback->viewport_rect().h>20&&!fallback_content->layout.has_box&&!fallback->children.front()->is_element("summary"),
       "Summaryless details show a layout-only label");
 check(details.key("Tab")&&details.document()->focused_id==fallback->id&&details.key("Enter")&&
       fallback->has_attr("open")&&fallback_content->layout.has_box,"Keyboard opens summaryless details");
 check(details.key(" ")&&!fallback->has_attr("open"),"Keyboard closes summaryless details");
 const auto fallback_box=fallback->viewport_rect();
 check(details.click(fallback_box.x+5,fallback_box.y+fallback_box.h-2)&&!fallback->has_attr("open"),
       "Clicking outside the fallback label does not activate it");
 check(details.click(fallback_box.x+5,fallback_box.y+5)&&fallback->has_attr("open")&&
       fallback_content->layout.has_box,"Fallback summary label responds to pointer input");
 auto* inline_details=node_in_details("inline-fallback");
 auto* inline_content=node_in_details("inline-content");
 const auto inline_box=inline_details->viewport_rect();
 check(inline_box.w>0&&inline_box.h>0&&!inline_content->layout.has_box,
       "Inline summaryless details has a visible fallback label");
 check(details.click(inline_box.x+3,inline_box.y+3)&&inline_details->has_attr("open")&&
       inline_content->layout.has_box,"Inline fallback label responds to pointer input");
 check(details.key(" ")&&!inline_details->has_attr("open")&&!inline_content->layout.has_box,
       "Inline fallback label supports keyboard activation");
 Engine modal(640,480);modal.set_wait_ms(0);
 check(modal.load((directory/"modal-backdrop.html").u8string(),&error)&&modal.render(false),"Modal backdrop fixture");
 auto color_at=[&](int x,int y,int r,int g,int b){
     const auto& data=modal.canvas()->pixels();
     const size_t pixel=(y*640+x)*4;
     return data[pixel]==r&&data[pixel+1]==g&&data[pixel+2]==b;
 };
 check(color_at(0,0,255,255,255)&&modal.click(20,20)&&modal.render(false),"Open native modal");
 check(color_at(0,0,153,153,153)&&color_at(25,70,153,0,0),"Author backdrop dims the viewport");
 check(color_at(185,56,0,0,255),"Modal paints above a higher z-index page element");
 check(modal.inspect("behind").find("\"center_hit\":false")!=std::string::npos,"Modal blocks background targeting");
 check(modal.click(110,52)&&modal.render(false)&&color_at(0,0,255,255,255),
       "Top-layer button closes dialog and restores background");
 check(modal.click(20,20)&&modal.render(false)&&
       color_at(224,58,0,255,0)&&modal.inspect("fixed-close").find("\"center_hit\":true")!=std::string::npos,
       "Viewport-fixed modal control is targetable outside the dialog box");
 check(modal.click(235,60)&&modal.render(false)&&color_at(0,0,255,255,255),
       "Viewport-fixed modal control closes the dialog");
 check(modal.click(20,20)&&modal.key("End")&&modal.render(false)&&color_at(0,0,255,255,255)&&
       color_at(405,56,0,0,255),"Detached and reinserted closed dialog paints normally");
 check(modal.document()->modal_dialogs.empty(),"Detachment clears native top-layer state");
 if(report)std::cout<<"SCROLL_RESULT {\"target\":["<<r.x<<","<<r.y<<"],\"sticky\":["<<s.x<<","<<s.y<<"],\"extent\":[400,700],\"window\":400}\n";
 std::cerr<<"MiniWeb scroll regressions passed\n";
}
