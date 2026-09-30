unit OleScalarInteger;

interface

function FindId(const Codice: string): Integer;

implementation

function FindId(const Codice: string): Integer;
var
  Rowset: TCSEOLEDBRowset;
  Accessor: TOLEDBAccessor;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
  try
    Accessor := Rowset.CreateDynamicAccessor;
    Rowset.QueryText := 'SELECT ID FROM Causali WHERE Codice = ''' + Codice + '''';
    Rowset.Open;
    Result := Accessor.Bindings[0].AsInteger;
  finally
    Rowset.Free;
  end;
end;

end.
