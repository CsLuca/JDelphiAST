unit OleConsumer;

interface

procedure Run;

implementation

procedure Run;
var
  Rowset: TCSEOLEDBRowset;
begin
  Rowset := TCSEOLEDBRowset.Create(nil);
end;

end.
