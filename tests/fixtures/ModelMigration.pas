unit ModelMigration;

interface

uses CSStdExt;

procedure Run;

implementation

procedure Run;
var
  CampiAssInt: TCSFields;
begin
  CampiAssInt.EnableOnChange := False;
  CampiAssInt.FieldValues[0] := GetCsField(Application, 'CampiAssInt');
  ObjAssInt.CSSeek(CampiAssInt);
  ObjAssInt.CSModify(CampiAssInt);
  ObjAssInt.CSResetCampi(CampiAssInt);
end;

end.
