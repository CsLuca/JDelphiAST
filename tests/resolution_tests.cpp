#include "jdelphiast/queries.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
void require(bool ok, const char* message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
int main() {
  using namespace jdelphiast;
  IndexedUnit visible; visible.name = "Visible"; visible.complete = true;
  AstDeclaration integer; integer.name="Pick"; integer.kind="function"; integer.type="Integer";
  integer.parameters.push_back({"Value","Integer",{},""});
  auto string = integer; string.type="string"; string.parameters[0].type="string";
  visible.declarations={integer,string}; visible.symbols={"Pick"};
  auto hidden=visible; hidden.name="Hidden";
  auto folder=std::filesystem::temp_directory_path()/"jdelphiast-resolution-tests";
  std::filesystem::create_directories(folder);
  auto file=folder/"Consumer.pas";
  auto check=[&](std::string source, std::size_t line, bool resolved, const char* message) {
    { std::ofstream output(file); output<<source; }
    auto json=expressionJson(file,line,{visible,hidden});
    require(json.find(resolved ? "\"resolved\":true" : "\"resolved\":false")!=std::string::npos,message);
  };
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nbegin\nPick(42);\nend;\nend.",7,true,"integer overload exact; hidden unit excluded");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nbegin\nPick('text');\nend;\nend.",7,true,"string overload exact");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Value: Variant;\nbegin\nPick(Value);\nend;\nend.",8,false,"Variant cannot choose overload");
  check("unit Consumer;\ninterface\nuses Visible;\nfunction Pick(Value: Integer): Integer;\nimplementation\nprocedure Run;\nbegin\nPick(42);\nend;\nend.",8,false,"local callable shadows SDK callable");
  check("unit Consumer;\ninterface\nuses Visible, Hidden;\nimplementation\nprocedure Run;\nbegin\nPick(42);\nend;\nend.",7,false,"two imported identical owners ambiguous");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nbegin\nForeign.Pick(42);\nend;\nend.",7,false,"unknown receiver cannot bind imported free function");
  AstDeclaration baseMethod;
  baseMethod.name="TBase.EliminaNoTrans"; baseMethod.kind="function"; baseMethod.type="Boolean"; baseMethod.ownerType="TBase";
  baseMethod.parameters.push_back({"Id","Integer",{},""});
  visible.declarations.push_back(baseMethod);
  visible.inheritance.push_back({"TChild","class","TBase",{}});
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Obj: TChild;\nbegin\nObj.EliminaNoTrans(42);\nend;\nend.",8,true,"inherited receiver binds ancestor method");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Obj: TForeign;\nbegin\nObj.EliminaNoTrans(42);\nend;\nend.",8,false,"foreign EliminaNoTrans cannot bind ancestor");
  check("unit Consumer;\ninterface\nuses Visible;\nvar Obj: TChild;\nimplementation\nprocedure First;\nvar Obj: TForeign;\nbegin\nObj.EliminaNoTrans(42);\nend;\nprocedure Second;\nbegin\nObj.EliminaNoTrans(42);\nend;\nend.",9,false,"local receiver shadows global");
  check("unit Consumer;\ninterface\nuses Visible;\nvar Obj: TChild;\nimplementation\nprocedure First;\nvar Obj: TForeign;\nbegin\nObj.EliminaNoTrans(42);\nend;\nprocedure Second;\nbegin\nObj.EliminaNoTrans(42);\nend;\nend.",13,true,"foreign local in earlier routine does not leak");
  auto mutableParameter=integer; mutableParameter.name="Change"; mutableParameter.parameters[0].modifier="var";
  visible.declarations.push_back(mutableParameter);
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nbegin\nChange(42);\nend;\nend.",7,false,"var argument literal rejected");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Value: Integer;\nbegin\nChange(Value);\nend;\nend.",8,true,"var typed lvalue exact");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nbegin\nVisible.Pick(42);\nend;\nend.",7,true,"qualified unit identity resolves exact call");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Visible: TForeign;\nbegin\nVisible.Pick(42);\nend;\nend.",8,false,"local receiver shadows a unit qualifier");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure First;\nvar Value: Integer;\nbegin\nPick(Value);\nend;\nprocedure Second;\nvar Value: string;\nbegin\nPick(Value);\nend;\nend.",8,true,"argument type belongs to first scope, not last declaration");
  mutableParameter.name="Produce"; mutableParameter.parameters[0].modifier="out";
  visible.declarations.push_back(mutableParameter);
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nbegin\nProduce(42);\nend;\nend.",7,false,"out literal rejected");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Value: Integer;\nbegin\nProduce(Value);\nend;\nend.",8,true,"out typed lvalue exact");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nbegin\nPick();\nend;\nend.",7,false,"missing argument cannot invent a default");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Value: Integer;\nbegin\nwith Obj do\nPick(Value);\nend;\nend.",9,false,"with implicit owner requires review");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Outer;\nvar Value: Integer;\nprocedure Inner;\nbegin\nPick(Value);\nend;\nbegin\nPick(Value);\nend;\nend.",9,true,"nested routine can see enclosing local");
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Outer;\nvar Value: Integer;\nprocedure Inner;\nvar Value: string;\nbegin\nPick(Value);\nend;\nbegin\nPick(Value);\nend;\nend.",13,true,"nested shadow ends at inner body end");
  AstDeclaration property; property.name="THolder.Child"; property.kind="property";
  property.ownerType="THolder"; property.type="TChild"; visible.declarations.push_back(property);
  check("unit Consumer;\ninterface\nuses Visible;\nimplementation\nprocedure Run;\nvar Holder: THolder;\nbegin\nHolder.Child.EliminaNoTrans(42);\nend;\nend.",8,true,"typed property chain resolves ancestor method");
  check("unit Consumer;\ninterface\nuses Visible;\ntype TView = class\nprivate FObj: TChild;\npublic procedure Run;\nend;\nimplementation\nprocedure TView.Run;\nbegin\nFObj.EliminaNoTrans(42);\nend;\nend.",11,true,"method scope resolves own class field");
  std::filesystem::remove_all(folder);
}
