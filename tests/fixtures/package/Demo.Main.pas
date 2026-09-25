unit Demo.Main;

interface

uses
  SysUtils,
  Demo.Shared;

procedure Run(E: Exception);

implementation

procedure Run(E: Exception);
begin
  UseShared;
end;

end.
