unit LegacyRefs;

interface

uses UVariStd;

procedure Run;

implementation

procedure Run;
begin
  Add(Value);
  Create;
  Format('%s', [Value]);
  IndexOf(Value);
  FieldByName('Code');
  CS_VariantTo_SqlStr(Value);
  RegObjStdD2(Value);
  ArrayCopia(Value);
end;

end.
