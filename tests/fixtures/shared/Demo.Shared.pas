unit Demo.Shared;

interface

type
  TSharedWorker = class
  public
    Value: string;
    constructor Create(AOwner: TObject);
  end;

function MakeValue(AException: Exception; const S: string; Enabled: Boolean): string;

implementation

constructor TSharedWorker.Create(AOwner: TObject);
begin
end;

function MakeValue(AException: Exception; const S: string; Enabled: Boolean): string;
begin
  Result := S;
end;

end.
