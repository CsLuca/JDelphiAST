unit Demo.Main;

interface

uses
  SysUtils,
  Demo.Shared;

procedure Run(E: Exception);

implementation

procedure Run(E: Exception);
var
  Worker: TSharedWorker;
begin
  Worker := TSharedWorker.Create(nil);
  Worker.Value := MakeValue(E, '', True);
end;

end.
