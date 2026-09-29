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
  ArrayCopia(Value);
end;

end.
