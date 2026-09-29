unit DiagnosticCalls;

interface

uses PrdRT.Utils, CSData, Mask;

procedure Run(Azienda: TAziendaStd; DBExec: TCSEDatabase);

implementation

procedure Run(Azienda: TAziendaStd; DBExec: TCSEDatabase);
var
  L: Integer;
begin
  L := TUtils_Table.GetColumnMaxLength(Azienda, 'AnaLav', 'Codice');
  L := DBExec.ExecSql('select 1');
  ResolveThing(UnknownValue);
  Add(
    'CodLav',
    ftWideString,
    TUtils_Table.GetColumnMaxLength(Azienda, 'AnaLav', 'Codice'),
    False
  );
end;

end.
