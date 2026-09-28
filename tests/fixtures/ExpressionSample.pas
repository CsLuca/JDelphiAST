unit ExpressionSample;

interface

uses
  FireDAC.Comp.Client;

implementation

procedure Test;
var
  Query: TCSEQuery;
begin
  Query.Params.FindParam('X');
end;

end.
