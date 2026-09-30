unit OleScalarString;

interface

function FindBadge(const Codice: string): string;

implementation

function FindBadge(const Codice: string): string;
var
  Rowset: TCSEOLEDBRowset;
  Accessor: TOLEDBAccessor;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
  try
    Accessor := Rowset.CreateDynamicAccessor;
    Rowset.QueryText := 'SELECT Badge FROM Dipendenti WHERE Codice = ''' + Codice + '''';
    Rowset.Open;
    Result := Accessor.Bindings[0].AsOleDbString;
  finally
    Rowset.Free;
  end;
end;

end.
