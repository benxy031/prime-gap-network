// Narrow emergency recovery for the reproduced epoch3 Core20 -> Core22 state failure.
// All chain validation remains in the unchanged, fingerprinted Core22 executable.
using System;
using System.IO;
using System.Text;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Diagnostics;
using System.Threading;
using System.Collections;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Management;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;

class CoreRecovery272 {
 const string UiHash="1a85803470dcaa7bc4cd77d6567687b8acc054e4c5ebda923bd1ef2b52a10dea";
 const string CoreHash="85ea03b94ad284c267926cf6d3e0bdcb77f030a3667e59666de43276e80503af";
 const string Genesis="710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314";
 const string Candidate="8f10d820e15956f939c738a53e13d02685fb606284f2c33fb0414aa8970321a1";
 static JavaScriptSerializer Json=new JavaScriptSerializer {MaxJsonLength=1048576};
 static string runtime, data, core, cookie, journal; static int port; static Process child;
 static Dictionary<string,object> Obj(object value){return (Dictionary<string,object>)value;}
 static string S(Dictionary<string,object> v,string key){return v.ContainsKey(key)?Convert.ToString(v[key]):"";}
 static void Need(bool b,string error){if(!b)throw new Exception(error);}
 static string Hash(string path){using(var f=File.OpenRead(path))using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(f)).Replace("-","").ToLowerInvariant();}
 static string Full(string p){return Path.GetFullPath(p).TrimEnd('\\','/');}
 static bool Same(string a,string b){return String.Equals(Full(a),Full(b),StringComparison.OrdinalIgnoreCase);}
 static string Quote(string value){var b=new StringBuilder("\"");int slashes=0;foreach(char c in value){if(c=='\\'){slashes++;continue;}if(c=='"')b.Append('\\',slashes*2+1);else b.Append('\\',slashes);slashes=0;b.Append(c);}b.Append('\\',slashes*2);b.Append('"');return b.ToString();}
 static string Args(IEnumerable<string> args){return String.Join(" ",args.Select(Quote));}
 static string Run(string exe,params string[] args){
  var p=Process.Start(new ProcessStartInfo(exe,Args(args)){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
  var output=p.StandardOutput.ReadToEndAsync();var error=p.StandardError.ReadToEndAsync();
  if(!p.WaitForExit(15000)){p.Kill();throw new Exception("Horizon did not answer its local control request.");}
  Need(p.ExitCode==0,"Horizon refused its local control request. No database repair was started.");
  Need(output.Result.Length<=1048576,"Local response exceeds the recovery budget.");return output.Result;
 }
 [DllImport("shell32.dll",SetLastError=true)]static extern IntPtr CommandLineToArgvW([MarshalAs(UnmanagedType.LPWStr)]string cmd,out int count);
 [DllImport("kernel32.dll")]static extern IntPtr LocalFree(IntPtr p);
 static string[] OriginalArguments(Process ui){
  string command;using(var p=new ManagementObject("Win32_Process.Handle='"+ui.Id+"'"))command=Convert.ToString(p["CommandLine"]);
  int count;var ptr=CommandLineToArgvW(command,out count);Need(ptr!=IntPtr.Zero&&count>=1,"The original Horizon launch could not be recovered.");
  try {var values=new string[count];for(int n=0;n<count;n++)values[n]=Marshal.PtrToStringUni(Marshal.ReadIntPtr(ptr,n*IntPtr.Size));return values.Skip(1).ToArray();}finally{LocalFree(ptr);}
 }
 static string Tail(string file){using(var f=new FileStream(file,FileMode.Open,FileAccess.Read,FileShare.ReadWrite)){f.Seek(Math.Max(0,f.Length-65536),SeekOrigin.Begin);using(var r=new StreamReader(f,Encoding.UTF8))return r.ReadToEnd();}}
 static object Rpc(string method,params object[] args){
  var request=(HttpWebRequest)WebRequest.Create("http://127.0.0.1:"+port+"/");request.Proxy=null;request.Method="POST";request.Timeout=120000;request.ReadWriteTimeout=120000;request.KeepAlive=false;
  request.Headers[HttpRequestHeader.Authorization]="Basic "+Convert.ToBase64String(Encoding.ASCII.GetBytes(File.ReadAllText(cookie).Trim()));request.ContentType="application/json";
  var bytes=Encoding.UTF8.GetBytes(Json.Serialize(new {jsonrpc="2.0",id="pgn-legacy-recovery272",method=method, @params=args}));request.ContentLength=bytes.Length;
  using(var stream=request.GetRequestStream())stream.Write(bytes,0,bytes.Length);
  WebResponse response;try{response=request.GetResponse();}catch(WebException e){response=e.Response;if(response==null)throw;}
  using(response)using(var reader=new StreamReader(response.GetResponseStream())){string raw=reader.ReadToEnd();Need(raw.Length<=1048576,"Core response exceeds the recovery budget.");var v=Obj(Json.DeserializeObject(raw));if(v.ContainsKey("error")&&v["error"]!=null)throw new Exception("Core: "+S(Obj(v["error"]),"message"));return v["result"];}
 }
 static Dictionary<string,string> WalletHashes(){
  var result=new Dictionary<string,string>();var paths=new List<string>();string folder=Path.Combine(data,"testnet","wallets");
  if(Directory.Exists(folder))paths.AddRange(Directory.GetFiles(folder,"wallet.dat",SearchOption.AllDirectories));string legacy=Path.Combine(data,"testnet","wallet.dat");if(File.Exists(legacy))paths.Add(legacy);
  Need(paths.Count<=64,"The wallet layout needs individual review; no repair was started.");foreach(string p in paths)result[p]=Hash(p);return result;
 }
 static void SaveJournal(string phase,string profileHash){
  var v=new Dictionary<string,object>{{"schema","PGN_LEGACY_CORE_RECOVERY_272"},{"phase",phase},{"profile_sha256",profileHash},{"core_sha256",CoreHash},{"data_directory",data},{"genesis",Genesis},{"rpc_port",port}};
  if(child!=null){v["pid"]=child.Id;v["birth"]=child.StartTime.ToUniversalTime().ToFileTimeUtc().ToString();}
  string tmp=journal+".pending";File.WriteAllText(tmp,Json.Serialize(v),new UTF8Encoding(false));if(File.Exists(journal))File.Replace(tmp,journal,null);else File.Move(tmp,journal);
 }
 static int Main(string[] args){
  FileStream lease=null;string lockfile=null;bool success=false;string reportPath=null;Dictionary<string,string> wallets=null;
  try {
   Console.WriteLine("PGN Horizon - legacy Core database recovery");Console.WriteLine("Wallets remain closed during reconstruction. No file is uploaded.");
   int requested=0;if(args.Length==2&&args[0]=="--ui-pid")requested=Int32.Parse(args[1]);else Need(args.Length==0,"Unsupported recovery arguments.");
   var candidates=Process.GetProcessesByName("PGN-Horizon").Where(p=>p.SessionId==Process.GetCurrentProcess().SessionId&&(requested==0||p.Id==requested)).ToArray();Need(candidates.Length==1,"Keep one Horizon window open on the stopped Core error, then run recovery again.");
   var ui=candidates[0];string exe=ui.MainModule.FileName;long birth=ui.StartTime.ToUniversalTime().ToFileTimeUtc();Need(Hash(exe)==UiHash,"This recovery supports the corrected UI160 package. No changes were made.");
   var state=Obj(Json.DeserializeObject(Run(exe,"--inspect-owned",ui.Id.ToString())));Need(!ui.HasExited&&ui.StartTime.ToUniversalTime().ToFileTimeUtc()==birth,"Horizon changed during inspection.");
   Need(S(state,"pgn_core_link_state")=="stopped","This recovery applies only to the reproduced stopped Core failure. No changes were made.");
   runtime=Full(S(state,"pgn_update_core_runtime_directory"));Need(Directory.Exists(runtime),"The selected Core configuration is unavailable.");string profilePath=Path.Combine(runtime,"wallet-core.json");string profileHash=Hash(profilePath);var profile=Obj(Json.DeserializeObject(File.ReadAllText(profilePath)));
   Need(S(profile,"chain")=="test"&&S(profile,"genesis_hash")==Genesis,"The selected wallet is for another network.");
   core=Full(S(profile,"executable"));data=Full(S(profile,"data_directory"));Need(Hash(core)==CoreHash&&S(profile,"executable_sha256")==CoreHash,"The selected configuration has not adopted Core22. No repair was applied.");
   Need(Directory.Exists(Path.Combine(data,"testnet","blocks"))&&Full(Path.GetPathRoot(data))!=data,"The retained blockchain directory could not be verified.");
   cookie=Path.Combine(data,"testnet",".cookie");journal=Path.Combine(runtime,"core-recovery272.json");reportPath=Path.Combine(runtime,"core-recovery272-result.json");
   string log=Tail(Path.Combine(data,"testnet","debug.log"));int start=log.LastIndexOf("Using data directory ",StringComparison.Ordinal);Need(start>=0,"The latest Core startup log is unavailable. No repair was applied.");log=log.Substring(start);
   Need(log.Contains("Native PGN database recheck held: native reconnect proof/history rejected"),"The local log reports a different startup failure. No repair was applied.");
   Need(new DriveInfo(Path.GetPathRoot(data)).AvailableFreeSpace>=536870912,"Free at least 512 MiB on the blockchain drive before recovery.");
   string[] restart=OriginalArguments(ui);Console.WriteLine("1/3  Closing the stopped Horizon session normally...");Run(exe,"--close-owned",ui.Id.ToString());Need(ui.WaitForExit(60000),"Horizon is still closing; no database repair was started.");Need(Hash(profilePath)==profileHash,"The selected configuration changed while closing.");
   lockfile=Path.Combine(runtime,"wallet-owned.lock");lease=new FileStream(lockfile,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.None);var lockBytes=Encoding.UTF8.GetBytes(Process.GetCurrentProcess().Id+"\nPGN-Core-Recovery\n"+Environment.MachineName+"\n");lease.Write(lockBytes,0,lockBytes.Length);lease.Flush(true);
   Need(!File.Exists(cookie),"A Core process still owns this data directory. No second Core was launched.");wallets=WalletHashes();
   var reserve=new TcpListener(IPAddress.Loopback,0);reserve.Start();port=((IPEndPoint)reserve.LocalEndpoint).Port;reserve.Stop();reserve=new TcpListener(IPAddress.Loopback,0);reserve.Start();int peerPort=((IPEndPoint)reserve.LocalEndpoint).Port;reserve.Stop();if(peerPort==port)peerPort=48951;
   SaveJournal("prepared",profileHash);Console.WriteLine("2/3  Reconstructing the local chain state with verified Core22...");
   var command=new[]{"-datadir="+data,"-testnet=1","-server=1","-rpcbind=127.0.0.1","-rpcallowip=127.0.0.1","-rpcport="+port,"-port="+peerPort,"-listen=0","-connect=0","-dnsseed=0","-fixedseeds=0","-discover=0","-disablewallet=1","-par=1","-dbcache=64","-reindex-chainstate=1"};
   child=Process.Start(new ProcessStartInfo(core,Args(command)){UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=Path.GetDirectoryName(core)});SaveJournal("rebuilding",profileHash);var clock=Stopwatch.StartNew();Dictionary<string,object> chain=null;
   while(!child.HasExited&&clock.ElapsedMilliseconds<180000){try{chain=Obj(Rpc("getblockchaininfo"));break;}catch{Thread.Sleep(200);}}
   Need(chain!=null&&!child.HasExited,"Core reconstruction did not reach a verified RPC state. The wallet was not opened.");Need(Convert.ToString(Rpc("getblockhash",0))==Genesis,"Core returned another genesis.");
   bool known=false;try{var header=Obj(Rpc("getblockheader",Candidate));known=Convert.ToInt32(header["height"])==4246&&S(header,"hash")==Candidate;}catch{}
   if(known)Rpc("reconsiderblock",Candidate); // Normal Core validation reruns; no acceptance rule is bypassed.
   Need(Convert.ToBoolean(Rpc("verifychain",4,50)),"Core's native database verification failed.");chain=Obj(Rpc("getblockchaininfo"));var utxo=Obj(Rpc("gettxoutsetinfo","muhash"));Rpc("stop");Need(child.WaitForExit(60000)&&child.ExitCode==0,"Core did not complete its normal shutdown.");
   foreach(var w in wallets)Need(File.Exists(w.Key)&&Hash(w.Key)==w.Value,"Wallet file integrity changed; Horizon was not restarted.");Need(Hash(profilePath)==profileHash,"The selected configuration changed during recovery.");SaveJournal("verified",profileHash);
   var receipt=new {PASS=true,at=DateTime.UtcNow.ToString("o"),height=chain["blocks"],tip=chain["bestblockhash"],verifychain4_50=true,wallet_files_unchanged=wallets.Count,core_sha256=CoreHash,profile_unchanged=true,utxo_muhash=utxo["muhash"]};File.WriteAllText(reportPath,Json.Serialize(receipt),new UTF8Encoding(false));
   lease.Dispose();lease=null;File.Delete(lockfile);lockfile=null;
   Console.WriteLine("3/3  Restarting Horizon with its original configuration...");Process.Start(new ProcessStartInfo(exe,Args(restart)){UseShellExecute=false,WorkingDirectory=Path.GetDirectoryName(exe)});success=true;
   Console.WriteLine("Recovery verified. Horizon can now reconnect and catch up with the network.");return 0;
  }catch(Exception e){Console.Error.WriteLine("Recovery stopped: "+e.Message);Console.Error.WriteLine("No wallet was deleted or replaced.");if(reportPath!=null)try{File.WriteAllText(reportPath,Json.Serialize(new {PASS=false,at=DateTime.UtcNow.ToString("o"),error=e.Message}),new UTF8Encoding(false));}catch{}return 1;
  }finally{
   if(child!=null&&!child.HasExited){try{Rpc("stop");child.WaitForExit(30000);}catch{}if(!child.HasExited)Console.Error.WriteLine("The recovery Core is still running. Its exact process is retained in core-recovery272.json; do not start another recovery.");}
   if(lease!=null){lease.Dispose();if(child==null||child.HasExited)try{File.Delete(lockfile);}catch{}}
   if(!success&&args.Length==0){Console.WriteLine("Press Enter to close.");Console.ReadLine();}
  }
 }
}
