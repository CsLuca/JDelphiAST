unit DiagnosticCalls;

interface

uses PrdRT.Utils, CSData;

procedure Run;

implementation

procedure Run;
var
  Azienda: TAziendaStd;
  L: Integer;
begin
  L := TUtils_Table.GetColumnMaxLength(Azienda, 'AnaLav', 'Codice');
  ResolveThing(UnknownValue);
  Add(
    'CodLav',
    ftWideString,
    TUtils_Table.GetColumnMaxLength(Azienda, 'AnaLav', 'Codice'),
    False
  );
end;

end.
