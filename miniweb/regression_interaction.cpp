#include "src/engine.h"
#include <filesystem>
#include <stdexcept>
#include <iostream>
void check_interaction(const std::filesystem::path& directory,bool report){
 Loader::set_access_root(directory.u8string());Engine engine(640,480);engine.set_wait_ms(0);std::string error;
 auto check=[&](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
 check(engine.load((directory/"interaction.html").u8string(),&error)&&engine.render(false),"Interaction fixture load");
 for(const char* id:{"open","clear"}){auto r=engine.document()->get_element_by_id(id)->absolute_rect();auto i=(static_cast<int>(r.y+5)*640+static_cast<int>(r.x+5))*4;const auto& pixels=engine.canvas()->pixels();check(pixels[i]==240&&pixels[i+1]==240&&pixels[i+2]==240,"Positioned dialog controls must be painted");}
 check(!engine.type("unfocused"),"Unfocused typing must fail");
 check(engine.key("Tab")&&engine.type("你好")&&engine.key("Home")&&engine.type("A")&&engine.key("End")&&engine.key("Backspace"),"Text editing");
 check(engine.key("Tab")&&engine.type("!")&&engine.key("Enter")&&engine.type("line2"),"Textarea focus/input");
 check(engine.key("Tab")&&engine.key(" ")&&engine.key("Tab")&&engine.key("Enter")&&engine.render(false),"Keyboard form submit");
 auto* body=engine.document()->body;auto result=body->attr("data-result");if(report)std::cout<<"INTERACTION_RESULT "<<result<<"\n";
 check(result.find("\"name\":\"A你\"")!=std::string::npos,"Live input value");
 check(result.find("\"attr\":\"seed\"")!=std::string::npos,"Input default attribute unchanged");
 check(result.find("\"note\":\"!\\nline2draft\"")!=std::string::npos,"Textarea live value");
 check(result.find("\"checked\":true")!=std::string::npos&&result.find("\"submitted\":1")!=std::string::npos,"Checkbox and submit");
 check(result.find("change:name")!=std::string::npos&&result.find("input:note")!=std::string::npos,"Input/change events");
 check(result.find("focus:disabled")==std::string::npos,"Disabled excluded from focus");
 check(engine.key("Enter")&&engine.render(false)&&body->attr("data-result").find("\"submitted\":2")!=std::string::npos,"State survives repeated render");
 check(engine.key("Shift+Tab")&&engine.key(" ")&&engine.key("Tab")&&engine.key("Enter"),"Reverse tab/check");
 check(body->attr("data-result").find("\"checked\":false")!=std::string::npos,"Checkbox toggled twice");
 check(!engine.key("Unimplemented")&&!engine.type(std::string(8193,'x')),"Bounded/unsupported input");
 auto click=[&](const char* id){auto r=engine.document()->get_element_by_id(id)->absolute_rect();return engine.click(r.x+r.w/2,r.y+r.h/2);};
 check(click("open")&&engine.type("!")&&engine.type("blocked")&&engine.key("Escape")&&engine.render(false),"Settings interaction");
 check(body->attr("data-setting")=="!warm","Canceled beforeinput and Escape");
 check(click("open")&&engine.key("Tab")&&!engine.type("mutate")&&engine.key("Tab")&&engine.key("Enter")&&engine.render(false),"Readonly control and dialog close");
 check(body->attr("data-setting")=="!warm","Settings state survives close/reopen");
 check(engine.document()->get_element_by_id("agree")->has_attr("checked")==false,"Live checkbox must not rewrite checked attribute");
 check(click("name")&&engine.type("tail")&&click("stayfocus")&&engine.type("x"),"Canceled pointer default keeps focus");
 auto* name=engine.document()->get_element_by_id("name");auto previous=name->control_value;
 check(engine.key(" ")&&name->control_value==previous+" ","Space inserts into text controls");
 check(click("plain")&&engine.document()->focused_id==0,"Plain-content pointer press blurs input");
 check(body->attr("data-committed")==name->control_value&&!engine.type("unfocused"),"Blur commits value and prevents further typing");
 check(click("actionButton")&&engine.key("Enter")&&body->attr("data-action")=="clicked"&&body->attr("data-action-count")=="2"&&!body->has_attr("data-wrong-submit"),"Input button Enter activates without implicit submit");
 check(click("actionSubmit")&&engine.key("Enter")&&body->attr("data-submit-click")=="canceled"&&body->attr("data-submit-count")=="2"&&!body->has_attr("data-wrong-submit"),"Input submit Enter preserves canceled click");
 check(click("implicitText")&&engine.key("Enter")&&body->attr("data-default-clicks")=="1"&&!body->has_attr("data-implicit-wrong"),"Implicit submit activates cancelable default button");
 check(click("disableDefault")&&click("implicitText")&&engine.key("Enter")&&body->attr("data-default-clicks")=="1"&&!body->has_attr("data-implicit-wrong"),"Disabled default prevents implicit submission");
 check(click("checkLabel")&&engine.document()->get_element_by_id("labelCheck")->checked_value,"Label forwards checkbox activation");
 check(click("cancelLabel")&&engine.document()->get_element_by_id("labelCheck")->checked_value,"Canceled label must not activate checkbox");
 check(click("textLabel")&&engine.type("label focus")&&engine.document()->get_element_by_id("labelText")->control_value=="label focus","Label focuses associated input");
 check(click("nestedLabel")&&engine.document()->get_element_by_id("nestedCheck")->checked_value,"Nested label activates non-focusable checkbox");
 check(click("clear")&&engine.render(false),"Clear textarea live value");
 auto area=engine.document()->get_element_by_id("note")->absolute_rect();
 for(int y=static_cast<int>(area.y)+5;y<area.bottom()-5;++y)for(int x=static_cast<int>(area.x)+5;x<area.right()-5;++x){auto i=(y*640+x)*4;const auto& pixels=engine.canvas()->pixels();check(pixels[i]==255&&pixels[i+1]==255&&pixels[i+2]==255,"Cleared textarea must not paint default text");}
 std::cerr<<"MiniWeb interaction regressions passed\n";
}
