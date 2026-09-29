unit ApiCalls;

interface

uses CSData, FireDAC.Comp.Client;

procedure Run;

implementation

procedure Run;
var
  AObjDB: TCSEDatabase;
  Qry: TFDQuery;
  LMsgErr: string;
  LStrSQL: string;
  numRec: Integer;
begin
  LMsgErr := AObjDB.ExecSql(LStrSQL);
  numRec := Qry.ExecSQL;
end;

end.
