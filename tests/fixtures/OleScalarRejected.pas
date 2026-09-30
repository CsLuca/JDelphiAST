unit OleScalarRejected;

interface

procedure FindLike(const Codice: string);
procedure FindCount(const Codice: string);
procedure FindLoop(const Codice: string);
procedure FindMultiBinding(const Codice: string);
procedure FindBlob(const Codice: string);

implementation

procedure FindLike(const Codice: string);
var
  Rowset: TCSEOLEDBRowset;
  Accessor: TOLEDBAccessor;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
  Accessor := Rowset.CreateDynamicAccessor;
  Rowset.QueryText := 'SELECT ID FROM Causali WHERE Codice LIKE ''' + Codice + '%''';
  Rowset.Open;
  Accessor.Bindings[0].AsInteger;
end;

procedure FindCount(const Codice: string);
var
  Rowset: TCSEOLEDBRowset;
  Accessor: TOLEDBAccessor;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
  Accessor := Rowset.CreateDynamicAccessor;
  Rowset.QueryText := 'SELECT Badge FROM Dipendenti WHERE Codice = ''' + Codice + '''';
  Rowset.Open;
  if Rowset.RecordCount > 0 then
    Accessor.Bindings[0].AsOleDbString;
end;

procedure FindLoop(const Codice: string);
var
  Rowset: TCSEOLEDBRowset;
  Accessor: TOLEDBAccessor;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
  Accessor := Rowset.CreateDynamicAccessor;
  Rowset.QueryText := 'SELECT Badge FROM Dipendenti WHERE Codice = ''' + Codice + '''';
  Rowset.Open;
  while not Rowset.Eof do
    Accessor.Bindings[0].AsOleDbString;
end;

procedure FindMultiBinding(const Codice: string);
var
  Rowset: TCSEOLEDBRowset;
  Accessor: TOLEDBAccessor;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
  Accessor := Rowset.CreateDynamicAccessor;
  Rowset.QueryText := 'SELECT ID, Badge FROM Dipendenti WHERE Codice = ''' + Codice + '''';
  Rowset.Open;
  Accessor.Bindings[0].AsInteger;
  Accessor.Bindings[1].AsOleDbString;
end;

procedure FindBlob(const Codice: string);
var
  Rowset: TCSEOLEDBRowset;
  Accessor: TOLEDBAccessor;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
  Accessor := Rowset.CreateDynamicAccessor;
  Rowset.QueryText := 'SELECT Immagine FROM Articoli WHERE Codice = ''' + Codice + '''';
  Rowset.Open;
  Accessor.Bindings[0].AsOleDbBinary;
end;

end.
