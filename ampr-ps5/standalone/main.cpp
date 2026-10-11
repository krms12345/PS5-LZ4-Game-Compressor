// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone application service. No PS5-Game-Compressor application modules.
#include "http.h"
#include "original.h"
#include "thread.h"
#include "assets.h"
#include "../src/ampr_backend.h"
#include "../../ampr-native/third_party/config/json.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <atomic>
#include <chrono>
#include <sys/statvfs.h>
#include <sys/stat.h>
#if defined(__FreeBSD__)||defined(__APPLE__)
#include <sys/param.h>
#include <sys/mount.h>
#endif
#include <signal.h>
using J=nlohmann::json;
namespace fs=std::filesystem;
using namespace studio;
#ifndef STUDIO_PORT
#define STUDIO_PORT 5911
#endif
#ifndef SHADOW_PORT
#define SHADOW_PORT 10101
#endif
#ifndef AMPR_STATE_ROOT
#define AMPR_STATE_ROOT "/data/AMPRStudio"
#endif
static std::mutex control;
static std::mutex stateLock;
static std::atomic<bool> abortJob{false};
#ifndef STUDIO_HOST_TEST
extern "C" int sceSystemServiceGetAppIdOfRunningBigApp(void);
extern "C" int sceSystemServiceGetAppTitleId(int,char*);
#endif
static bool gameRunning(){
#ifndef STUDIO_HOST_TEST
    int app=sceSystemServiceGetAppIdOfRunningBigApp();if(app<0)return false;
    char title[32]={};if(sceSystemServiceGetAppTitleId(app,title)<0)return true;
    return std::string(title).rfind("PPSA",0)==0||std::string(title).rfind("CUSA",0)==0;
#else
    return false;
#endif
}
static J state={{"ok",true},{"active",false},{"started",false},{"result",0},{"phase","Ready"},{"done",0},{"total",0},{"stagePercent",0.0},{"benchmark",false}};
static std::string logTail;
static auto began=std::chrono::steady_clock::now(),stageBegan=began;
static double seconds(std::chrono::steady_clock::time_point t){return std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();}
static std::string get(const Request&r,const std::string& key,const std::string& fallback=""){auto i=r.query.find(key);return i==r.query.end()?fallback:i->second;}
static int integer(const Request&r,const std::string& key,int fallback,int maximum=12){auto s=get(r,key,std::to_string(fallback));int n=int(decimal(s,size_t(maximum)));require(n>0,"Invalid "+key);return n;}
static J shadow(const std::string& operation,const J& body=J::object()){
    auto response=postLoopback(SHADOW_PORT,"/api/v1/"+operation,body.dump());
    auto json=J::parse(response.body);require(response.first.find(" 200 ")!=std::string::npos&&json.value("status",-1)==0,"ShadowMount "+operation+" failed: "+response.body);return json;
}
static bool readOnly(const fs::path& root){struct statvfs info{};return fs::is_directory(root)&&statvfs(root.c_str(),&info)==0&&(info.f_flag&ST_RDONLY);}
static uint64_t freeSpace(const fs::path& root){struct statvfs info{};require(statvfs(root.c_str(),&info)==0,"Cannot measure destination space: "+root.string());require(!(info.f_flag&ST_RDONLY),"Destination is read-only");require(info.f_frsize&&info.f_bavail<=UINT64_MAX/info.f_frsize,"Invalid free-space result");return uint64_t(info.f_bavail)*info.f_frsize;}
static bool contains(const fs::path& root,const fs::path& path){auto a=fs::weakly_canonical(root),b=fs::weakly_canonical(path);auto i=a.begin(),k=b.begin();while(i!=a.end()&&k!=b.end()&&*i==*k){++i;++k;}return i==a.end();}
// Writable drives besides the built-in SSD. ext1 is the M.2 expansion SSD and ext0 is USB extended storage;
// both default to a homebrew folder, matching where game scanners look. USB drives keep the root as before.
struct ExternalDrive {fs::path root;std::string name;fs::path target;};
static std::vector<ExternalDrive> externalDrives(){std::vector<ExternalDrive> drives{{"/mnt/ext1","M.2 SSD","/mnt/ext1/homebrew"},{"/mnt/ext0","External SSD","/mnt/ext0/homebrew"}};for(int n=0;n<8;n++){fs::path usb="/mnt/usb"+std::to_string(n);drives.push_back({usb,"USB "+std::to_string(n),usb});}
#ifdef STUDIO_HOST_TEST
    if(const char* test=getenv("STUDIO_TEST_DRIVE"))drives.push_back({test,"Test drive",fs::path(test)/"homebrew"});
#endif
    return drives;}
// Mount points can pass through the device ID of the layer beneath, so accept any of: a mount-table entry, a new device ID or a new filesystem ID.
static bool mountedDrive(const fs::path& root){
    if(!fs::is_directory(root))return false;
#ifdef STUDIO_HOST_TEST
    if(const char* test=getenv("STUDIO_TEST_DRIVE");test&&root==test)return true;
#endif
#if defined(__FreeBSD__)||defined(__APPLE__)
    int count=getfsstat(nullptr,0,MNT_NOWAIT);if(count>0){std::vector<struct statfs> list(size_t(count)+8);count=getfsstat(list.data(),long(list.size()*sizeof(struct statfs)),MNT_NOWAIT);for(int n=0;n<count;n++)if(root==list[size_t(n)].f_mntonname)return true;}
#endif
    struct stat device{},parent{};if(stat(root.c_str(),&device)==0&&stat(root.parent_path().c_str(),&parent)==0&&device.st_dev!=parent.st_dev)return true;
    struct statvfs a{},b{};return statvfs(root.c_str(),&a)==0&&statvfs(root.parent_path().c_str(),&b)==0&&a.f_fsid!=b.f_fsid;
}
static bool driveAvailable(const fs::path& root){struct statvfs v{};return mountedDrive(root)&&statvfs(root.c_str(),&v)==0&&!(v.f_flag&ST_RDONLY)&&v.f_frsize&&v.f_blocks<=UINT64_MAX/v.f_frsize&&uint64_t(v.f_blocks)*v.f_frsize>=uint64_t(64)*1024*1024;}
static bool onExternalDrive(const fs::path& path){for(auto& drive:externalDrives())if(contains(drive.root,path)&&driveAvailable(drive.root))return true;return false;}
static std::string storageName(const std::string& path){if(path.rfind("/data/",0)==0)return "Internal SSD";for(auto& drive:externalDrives())if(path.rfind(drive.root.string()+"/",0)==0)return drive.name;return "Other storage";}
// Unpacked game dumps: folders with sce_sys/param.json and eboot.bin in the usual homebrew locations.
// Folders holding pack metadata are converted output (or a failed one) and are not offered as sources.
static bool gameFolder(const fs::path& dir){std::error_code ec;return !fs::is_symlink(dir,ec)&&fs::is_directory(dir,ec)&&fs::is_regular_file(dir/"sce_sys/param.json",ec)&&fs::is_regular_file(dir/"eboot.bin",ec)&&!fs::exists(dir/"ampr_assets.index",ec)&&!fs::exists(dir/"INCOMPLETE.txt",ec);}
static bool ps5TitleId(const std::string& id){return id.size()==9&&id.substr(0,4)=="PPSA"&&std::all_of(id.begin()+4,id.end(),[](char c){return c>='0'&&c<='9';});}
static std::vector<J> folderGames(){
    std::vector<fs::path> roots{"/data/homebrew","/data/etaHEN/games"};
    for(auto& drive:externalDrives())if(mountedDrive(drive.root))for(auto& root:{drive.root,drive.root/"homebrew",drive.root/"etaHEN/games"})if(std::find(roots.begin(),roots.end(),root)==roots.end())roots.push_back(root);
    std::vector<J> games;std::error_code ec;
    for(auto& root:roots){for(auto& entry:fs::directory_iterator(root,ec)){if(!gameFolder(entry.path()))continue;try{std::ifstream file(entry.path()/"sce_sys/param.json");auto param=J::parse(file);auto id=param.value("titleId",std::string());if(!ps5TitleId(id))continue;std::string name=id;if(param.contains("localizedParameters")){auto& local=param["localizedParameters"];auto language=local.value("defaultLanguage",std::string("en-US"));if(local.contains(language)&&local[language].is_object())name=local[language].value("titleName",id);}
        games.push_back(J{{"title_id",id},{"title_name",name},{"path",entry.path().string()},{"runtime_path",entry.path().string()},{"mounted",true},{"source_kind","folder"}});}catch(const std::exception&){}}}
    return games;
}
static J shadowGames(){try{return shadow("games").at("games");}catch(const std::exception&){return J::array();}}
static J findGame(const Request&r){auto id=get(r,"titleId"),path=get(r,"sourcePath");require(ps5TitleId(id),"Invalid title ID");for(auto& game:folderGames())if(game.value("title_id","")==id&&game.value("path","")==path)return game;auto listing=shadow("games");for(auto& game:listing.at("games")){if(game.value("title_id","")==id&&game.value("path","")==path&&game.value("managed",false)&&game.value("source_available",false))return game;}throw std::runtime_error("The selected source is no longer available");}
static fs::path destination(const std::string& input,const fs::path& source){fs::path path(input);require(path.is_absolute()&&path!=path.root_path(),"Choose an absolute new destination folder");require(!contains(source,path),"Destination cannot be inside the mounted game");require(!fs::exists(path),"Destination already exists; choose a new folder");auto canonical=fs::weakly_canonical(path);bool allowed=contains("/data/homebrew",canonical)||onExternalDrive(canonical);require(allowed,"Choose an internal homebrew folder, the M.2 SSD, an external SSD or a connected USB drive");fs::create_directories(canonical.parent_path());require(!fs::exists(canonical),"Destination already exists");return canonical;}
static bool option(const Request&r,const std::string& key){auto value=get(r,key,"yes");require(value=="yes"||value=="no","Invalid "+key);return value=="yes";}
static fs::path backupFolder(const Request&r,const fs::path& source,const fs::path& output){
    fs::path folder=get(r,"originalBackup","/data/game-backups");require(folder.is_absolute(),"Choose an absolute original backup folder");folder=fs::weakly_canonical(folder);
    bool allowed=contains("/data/game-backups",folder)||onExternalDrive(folder);
    require(allowed&&!contains(source,folder)&&!contains(output,folder)&&!contains(folder,output),"Choose a separate backup folder on internal storage, the M.2 SSD, an external SSD or USB");
    fs::create_directories(folder);return folder;
}
static bool folderSource(const J& game){return game.value("source_kind","")=="folder";}
static J plan(const Request&r,bool measure=true){
    require(!gameRunning(),"Close the running game before conversion");auto game=findGame(r);std::string root=game.value("runtime_path","");bool folder=folderSource(game);auto mode=get(r,"mode","internal");require(mode=="internal"||mode=="usb"||mode=="usb_then_internal"||mode=="swap","Invalid destination mode");integer(r,"level",7);bool verifyOutput=option(r,"verifyOutput"),verifyCopy=option(r,"verifyCopy");auto original=get(r,"originalAction","keep");bool swapping=original=="swap";if(swapping){original="move";require(mode=="swap"&&verifyOutput&&verifyCopy,"Swap requires full conversion and copy verification");}else require(mode!="swap","Swap mode requires Swap original and converted");require(original=="keep"||original=="move"||original=="delete","Invalid original-file action");require(!folder||(original=="keep"&&!swapping),"Folder dumps always keep the original; Move, Delete and Swap need an image source");
    J p={{"ok",true},{"engineReady",true},{"canStart",false},{"readOnlyVerified",false},{"logicalBytes",0},{"requiredOutputBytes",0},{"outputFreeBytes",0},{"internalFreeBytes",0},{"mountedRoot",root},{"mountExists",fs::is_directory(root)},{"message","Mount the source read-only before conversion."}};
    // A folder dump is not a read-only mount; it is fingerprinted instead and checked again after packing.
    p["sourceKind"]=folder?"folder":"image";if(!folder&&(!game.value("mounted",false)||!readOnly(root)))return p;p["readOnlyVerified"]=true;
    auto out=destination(get(r,"output"),root);require(mode=="swap"||(mode=="internal"?contains("/data/homebrew",out):!contains("/data/homebrew",out)),"Destination does not match selected storage mode");uint64_t logical=0,budget=0;char error[1024]={};
    if(measure){int rc=ampr_backend_measure(root.c_str(),&logical,&budget,error,sizeof(error));require(rc==0,error);}
    auto free=freeSpace(out.parent_path());p["outputFreeBytes"]=free;p["logicalBytes"]=logical;p["requiredOutputBytes"]=budget;p["output"]=out.string();
    bool enough=free>=budget;if(mode=="usb_then_internal"||mode=="swap"){auto internal=destination(get(r,"internal"),root);require(mode=="swap"||contains("/data/homebrew",internal),"Final copy must be on internal storage");if(mode=="swap"){auto identity=identifyOriginal(game.at("path").get<std::string>());struct stat target{},working{};require(stat(internal.parent_path().c_str(),&target)==0&&stat(out.parent_path().c_str(),&working)==0&&target.st_dev==identity.device&&working.st_dev!=identity.device,"Swap must use different drives and return the converted game to the original drive");}require(!contains(out,internal)&&!contains(internal,out),"Output folders overlap");auto internalFree=freeSpace(internal.parent_path());p["internalFreeBytes"]=internalFree;p["internal"]=internal.string();enough=enough&&internalFree>=budget;struct stat a{},b{};require(stat(out.parent_path().c_str(),&a)==0&&stat(internal.parent_path().c_str(),&b)==0,"Cannot inspect destination devices");require(a.st_dev!=b.st_dev,"USB working output and internal copy must be on different devices");}
    p["verifyOutput"]=verifyOutput;p["verifyCopy"]=verifyCopy;p["originalAction"]=original;
    bool verified=verifyOutput&&((mode!="usb_then_internal"&&mode!="swap")||verifyCopy);p["swapOriginal"]=swapping;p["verificationSkipped"]=!verified;
    if(original!="keep"){auto originalPath=fs::path(game.at("path").get<std::string>());auto identity=identifyOriginal(originalPath);require(originalPath!=out,"Original and output overlap");if(original=="move"){auto folder=backupFolder(r,root,out);if(mode=="usb_then_internal"||mode=="swap")require(!contains(fs::path(p.at("internal").get<std::string>()),folder)&&!contains(folder,fs::path(p.at("internal").get<std::string>())),"Backup and internal output folders overlap");require(!fs::exists(folder/originalPath.filename()),"Backup already exists; choose another folder");struct stat device{},outputDevice{},internalDevice{};require(stat(folder.c_str(),&device)==0&&stat(out.parent_path().c_str(),&outputDevice)==0,"Cannot inspect backup drive");if(swapping)require(device.st_dev==outputDevice.st_dev,"Swap must move the original to the conversion destination drive");if(device.st_dev!=identity.device){uint64_t needed=uint64_t(identity.size);if(device.st_dev==outputDevice.st_dev){require(budget<=UINT64_MAX-needed,"Space budget overflow");needed+=budget;}if(mode=="usb_then_internal"||mode=="swap"){require(stat(fs::path(p.at("internal").get<std::string>()).parent_path().c_str(),&internalDevice)==0,"Cannot inspect internal output drive");if(device.st_dev==internalDevice.st_dev){require(budget<=UINT64_MAX-needed,"Space budget overflow");needed+=budget;}}enough=enough&&freeSpace(folder)>=needed;}p["originalBackup"]=folder.string();}}
    p["canStart"]=enough;p["message"]=enough?(verified?"Plan ready. Selected original-file action runs after conversion.":"Plan ready. Verification will be skipped; output will be marked unverified."):"Not enough destination or backup space.";return p;
}
static void appendLog(const std::string& line){std::lock_guard<std::mutex> guard(stateLock);logTail+=line;if(logTail.size()>131072)logTail.erase(0,logTail.size()-131072);std::ofstream file(fs::path(AMPR_STATE_ROOT)/"logs/current.log",std::ios::app);file<<line;}
struct SourceGuard {fs::path path;dev_t device;ino_t inode;bool folder=false;std::chrono::steady_clock::time_point checked{};bool changed=false;std::mutex lock;};
static int cancellation(void* context){if(abortJob)return 1;auto& source=*static_cast<SourceGuard*>(context);std::lock_guard<std::mutex> guard(source.lock);if(seconds(source.checked)<1)return source.changed;source.checked=std::chrono::steady_clock::now();struct stat info{};if(gameRunning()||stat(source.path.c_str(),&info)||info.st_dev!=source.device||info.st_ino!=source.inode||(!source.folder&&!readOnly(source.path))){source.changed=true;appendLog("Game started or source mount changed. Stopping.\n");}return source.changed;}
// Size and modification time of every entry under a folder dump, used to detect changes during conversion.
static std::map<std::string,std::pair<uintmax_t,int64_t>> fingerprint(const fs::path& root){std::map<std::string,std::pair<uintmax_t,int64_t>> files;for(auto& entry:fs::recursive_directory_iterator(root)){struct stat info{};require(lstat(entry.path().c_str(),&info)==0,"Cannot read source entry: "+entry.path().string());files[entry.path().lexically_relative(root).generic_string()]={S_ISREG(info.st_mode)?uintmax_t(info.st_size):0,int64_t(info.st_mtime)};}return files;}
static void phaseCallback(void*,const char* value){std::lock_guard<std::mutex> guard(stateLock);std::string phase=value;if(state.value("swapOriginal",false)){if(phase=="Copying output to internal")phase="Returning converted game";if(phase=="Verifying internal copy")phase="Verifying returned game";}state["phase"]=phase;state["done"]=0;state["total"]=0;state["stagePercent"]=0.0;stageBegan=std::chrono::steady_clock::now();std::string s=value;state["progressUnit"]=s.find("Packing")!=std::string::npos?"files":"bytes";}
static void progressCallback(void*,uint64_t done,uint64_t total){std::lock_guard<std::mutex> guard(stateLock);state["done"]=done;state["total"]=total;state["stagePercent"]=total?std::min(100.0,100.0*done/total):0;}
static void logCallback(void*,const char* line){appendLog(line);std::string text=line;if(text.rfind("Packing ",0)==0){std::lock_guard<std::mutex> guard(stateLock);state["currentFile"]=text;}}
static J backendJson(void(*function)(char*,size_t),size_t limit=1024*1024){std::vector<char> bytes(limit);function(bytes.data(),bytes.size());return J::parse(bytes.data());}
static void restartShadowMount(const std::string& titleId){
    require(!abortJob&&!gameRunning(),"ShadowMount restart skipped: conversion canceled or a game is running");
    fs::path payload;std::ifstream autoload("/data/pldmgr/autoload.txt");std::string name;
    while(std::getline(autoload,name)){if(!name.empty()&&name.back()=='\r')name.pop_back();auto lower=name;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});if(lower.find("shadowmount")==std::string::npos||fs::path(name).filename()!=name||fs::path(name).extension()!=".elf")continue;
        for(const auto& entry:fs::recursive_directory_iterator("/data/pldmgr/payloads")){if(entry.path().filename()!=name||!entry.is_regular_file()||entry.is_symlink())continue;require(payload.empty(),"Multiple ShadowMount payloads found");payload=entry.path();}}
    require(!payload.empty(),"ShadowMount payload was not found in Payload Manager autoload");
    phaseCallback(nullptr,"Restarting ShadowMount");appendLog("Restarting ShadowMount after conversion.\n");
    auto listing=shadow("games");for(const auto& game:listing.at("games"))if(game.value("title_id","")==titleId&&game.value("mounted",false))shadow("games/unmount",J{{"title_id",titleId}});
    require(!abortJob&&!gameRunning(),"ShadowMount restart skipped: conversion canceled or a game is running");
    std::string encoded;const char* hex="0123456789ABCDEF";for(unsigned char c:payload.string()){if(std::isalnum(c)||c=='/'||c=='-'||c=='_'||c=='.')encoded+=char(c);else{encoded+='%';encoded+=hex[c>>4];encoded+=hex[c&15];}}
    auto response=getLoopback(8084,"/loadpayload:"+encoded);require(response.first.find(" 200 ")!=std::string::npos,"Payload Manager could not reload ShadowMount");
    // Allow the replacement instance to finish its graceful handoff before requesting a scan.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    for(int attempt=0;attempt<15;attempt++){try{shadow("scan");appendLog("ShadowMount restarted; library scan requested.\n");return;}catch(const std::exception&){if(attempt==14)throw;std::this_thread::sleep_for(std::chrono::seconds(1));}}
}
static void launch(const Request&r,bool benchmark){
    Request planning=r;if(benchmark){planning.query["originalAction"]="keep";if(get(planning,"mode")=="swap")planning.query["mode"]=get(planning,"output").rfind("/data/",0)==0?"internal":"usb";}auto p=plan(planning,!benchmark);require(p.value("canStart",false),p.value("message","Plan is unavailable"));
    std::string root=p.at("mountedRoot"),out=p.at("output"),internal=p.value("internal","");bool verifyOutput=p.value("verifyOutput",true),verifyCopy=p.value("verifyCopy",true);std::string original=p.value("originalAction","keep"),backup=p.value("originalBackup","");auto game=findGame(r);bool folder=folderSource(game);std::string originalPath=game.value("path","");std::string titleId=game.value("title_id","");OriginalIdentity originalIdentity{};if(!benchmark&&original!="keep")originalIdentity=identifyOriginal(originalPath);bool verified=verifyOutput&&(internal.empty()||verifyCopy);
    if(!benchmark)requireDeleteConsent(original,verified,get(r,"confirmOriginalDelete")=="yes",get(r,"confirmUnverifiedDelete")=="yes");
    bool swapping=p.value("swapOriginal",false);bool restartAfter=!benchmark&&get(r,"restartShadowMount","no")=="yes";require(get(r,"restartShadowMount","no")=="yes"||get(r,"restartShadowMount","no")=="no","Invalid ShadowMount restart option");int level=integer(r,"level",7);struct stat info{};require(stat(root.c_str(),&info)==0,"Source mount disappeared");
    auto runtime=fs::path(AMPR_STATE_ROOT)/"libSceAmpr.sprx";require(fs::is_regular_file(runtime),"AMPR runtime is unavailable");
    if(benchmark)require(freeSpace(fs::path(out).parent_path())>=uint64_t(2400)*1024*1024,"Performance test needs at least 2.2 GiB free");
    {std::lock_guard<std::mutex> guard(stateLock);state={{"ok",true},{"active",true},{"started",true},{"result",0},{"phase","Preparing"},{"done",0},{"total",0},{"stagePercent",0.0},{"benchmark",benchmark},{"destination",out},{"error",""},{"verified",false},{"verificationSkipped",!verified},{"originalAction",original},{"swapOriginal",swapping},{"verifyOutput",verifyOutput},{"verifyCopy",verifyCopy},{"copyToInternal",!internal.empty()},{"restartShadowMount",restartAfter},{"originalStatus","Pending"}};logTail.clear();began=stageBegan=std::chrono::steady_clock::now();std::ofstream(fs::path(AMPR_STATE_ROOT)/"logs/current.log",std::ios::trunc);}
    abortJob=false;if(benchmark)ampr_backend_benchmark_prepare();int depth=get(r,"depth","quick")=="full"?1:get(r,"depth")=="io"?2:0;
    startDetached([root,out,internal,level,runtime,benchmark,depth,info,folder,verifyOutput,verifyCopy,verified,original,originalPath,originalIdentity,backup,titleId,restartAfter,swapping](){SourceGuard source{root,info.st_dev,info.st_ino,folder};ampr_backend_callbacks callbacks{&source,cancellation,phaseCallback,progressCallback,logCallback};char error[2048]={};int result=-1;DirectoryIdentity workingIdentity{},returnedIdentity{};try{if(benchmark)result=ampr_backend_benchmark_game(root.c_str(),fs::path(out).parent_path().c_str(),level,depth,nullptr,&callbacks,error,sizeof(error));else{std::map<std::string,std::pair<uintmax_t,int64_t>> before;if(folder){phaseCallback(nullptr,"Recording source folder");before=fingerprint(root);}result=ampr_backend_pack_game_options(root.c_str(),out.c_str(),level,runtime.c_str(),nullptr,verifyOutput,&callbacks,error,sizeof(error));
            if(!result&&folder&&fingerprint(root)!=before){std::ofstream(fs::path(out)/"INCOMPLETE.txt")<<"The source folder changed during conversion; output is not approved.\n";throw std::runtime_error("The source folder changed during conversion; output marked incomplete. Close anything writing to the game folder and convert again.");}if(!result&&swapping)workingIdentity=identifyDirectory(out);if(!result&&!internal.empty())result=ampr_backend_copy_options(out.c_str(),internal.c_str(),verifyCopy,&callbacks,error,sizeof(error));if(!result&&swapping)returnedIdentity=identifyDirectory(internal);if(!result){std::lock_guard<std::mutex> guard(stateLock);state["verified"]=verified;}if(!result&&original!="keep"){require(!abortJob&&!gameRunning(),"Original retained: canceled or game started");shadow("games/unmount",J{{"title_id",titleId}});struct stat mounted{};require(stat(root.c_str(),&mounted)!=0||mounted.st_dev!=info.st_dev||mounted.st_ino!=info.st_ino||!readOnly(root),"Original retained: source is still mounted");handleOriginal(originalPath,originalIdentity,original,backup,[](){require(!abortJob&&!gameRunning(),"Original operation canceled; original retained if not already moved");},[](const char* name){phaseCallback(nullptr,name);},[](uint64_t done,uint64_t total){progressCallback(nullptr,done,total);});}if(!result&&swapping){phaseCallback(nullptr,"Finishing swap");finishSwap(out,workingIdentity,internal,returnedIdentity,originalPath,backup,originalIdentity,[](){require(!abortJob&&!gameRunning(),"Swap copies are ready; canceled or game started, so temporary converted copy retained");});{std::lock_guard<std::mutex> guard(stateLock);state["destination"]=internal;}appendLog("Swap completed: converted game returned to the original drive; original moved to the destination drive.\n");}if(!result){std::lock_guard<std::mutex> guard(stateLock);state["originalStatus"]=original=="delete"?"Deleted":original=="move"?"Moved":"Kept in place";}}if(!result&&restartAfter){try{restartShadowMount(titleId);std::lock_guard<std::mutex> guard(stateLock);state["shadowMountStatus"]="Restarted; library scan requested";}catch(const std::exception&e){appendLog(std::string("WARNING: Conversion completed, but ")+e.what()+"\n");std::lock_guard<std::mutex> guard(stateLock);state["shadowMountStatus"]=std::string("Warning: ")+e.what();}}}catch(const std::exception&e){result=-1;appendLog(std::string("ERROR: ")+e.what()+"\n");snprintf(error,sizeof(error),"%s",e.what());if(benchmark)ampr_backend_benchmark_failed(error);}std::lock_guard<std::mutex> guard(stateLock);state["active"]=false;state["result"]=result;state["error"]=error;state["phase"]=result?(abortJob?"Canceled":"Stopped"):(verified?"Complete: output verified":"Complete: output unverified");});
}
static J status(){J s;{std::lock_guard<std::mutex> guard(stateLock);s=state;s["elapsedSeconds"]=seconds(began);s["stageElapsedSeconds"]=seconds(stageBegan);}s["processId"]=getpid();s["performance"]=backendJson(ampr_backend_metrics,131072);if(s.value("phase",std::string()).find("original")!=std::string::npos)s["performance"]=J::object();s["partialFolders"]=backendJson(ampr_backend_partial_info,32768);return s;}
static J route(const Request&r){
    auto path=r.path;bool mutation=r.method=="POST";require(mutation||r.method=="GET","Unsupported method");
    if(path=="/api/gc/ampr-lz4/status")return status();
    if(path=="/api/control/handoff-state"){auto s=status();return J{{"ok",true},{"busy",s["active"]}};}
    if(path=="/api/gc/ampr-lz4/benchmark/status"){auto s=backendJson(ampr_backend_benchmark_state,8*1024*1024);s["ok"]=true;return s;}
    if(path=="/api/gc/job/cancel"){require(mutation,"Use POST to cancel");abortJob=true;return J{{"ok",true},{"message","Cancellation requested"}};}
    if(path=="/api/gc/games"){J games=J::array();for(auto& g:shadowGames()){if(!g.value("managed",false)||!g.value("source_available",false)||!g.value("image_backed",false))continue;games.push_back(J{{"name",g.value("title_name",g.value("title_id",""))},{"titleId",g.value("title_id","")},{"sourcePath",g.value("path","")},{"sourceStorage",storageName(g.value("path",""))},{"sourceKind","image"},{"readOnlyVerified",g.value("mounted",false)&&readOnly(g.value("runtime_path",std::string()))}});}for(auto& g:folderGames())games.push_back(J{{"name",g.value("title_name","")},{"titleId",g.value("title_id","")},{"sourcePath",g.value("path","")},{"sourceStorage",storageName(g.value("path",""))},{"sourceKind","folder"},{"readOnlyVerified",true}});return J{{"ok",true},{"games",games}};}
    if(path=="/api/gc/ampr-lz4/capacity"){J devices=J::array();auto drives=externalDrives();drives.insert(drives.begin(),{"/data","Internal SSD","/data/homebrew"});for(auto& drive:drives){bool internal=drive.root=="/data";J d={{"name",drive.name},{"path",drive.target.string()},{"available",false},{"freeBytes",0},{"totalBytes",0}};struct statvfs v{};if(fs::is_directory(drive.root)&&statvfs(drive.root.c_str(),&v)==0&&v.f_frsize&&v.f_blocks<=UINT64_MAX/v.f_frsize){d["available"]=internal?(v.f_blocks>0&&!(v.f_flag&ST_RDONLY)):driveAvailable(drive.root);d["freeBytes"]=uint64_t(v.f_bavail)*v.f_frsize;d["totalBytes"]=uint64_t(v.f_blocks)*v.f_frsize;}devices.push_back(d);}return J{{"ok",true},{"devices",devices}};}
    if(path=="/api/gc/ampr-lz4/mounts"){J mounts=J::array(),folders=J::array();
#if defined(__FreeBSD__)||defined(__APPLE__)
        int count=getfsstat(nullptr,0,MNT_NOWAIT);if(count>0){std::vector<struct statfs> list(size_t(count)+8);count=getfsstat(list.data(),long(list.size()*sizeof(struct statfs)),MNT_NOWAIT);for(int n=0;n<count;n++){auto& m=list[size_t(n)];mounts.push_back(J{{"mountedOn",m.f_mntonname},{"from",m.f_mntfromname},{"type",m.f_fstypename},{"readOnly",bool(m.f_flags&MNT_RDONLY)},{"totalBytes",uint64_t(m.f_blocks)*m.f_bsize},{"freeBytes",uint64_t(m.f_bavail)*m.f_bsize}});}}
#endif
        std::error_code ec;for(auto& entry:fs::directory_iterator("/mnt",ec))folders.push_back(J{{"path",entry.path().string()},{"mountPoint",mountedDrive(entry.path())}});
        J drives=J::array();for(auto& drive:externalDrives())drives.push_back(J{{"path",drive.root.string()},{"name",drive.name},{"exists",fs::is_directory(drive.root)},{"mountPoint",mountedDrive(drive.root)},{"readOnly",readOnly(drive.root)},{"available",driveAvailable(drive.root)}});
        return J{{"ok",true},{"drives",drives},{"mnt",folders},{"mounts",mounts}};}
    if(path=="/api/gc/ampr-lz4/size"){auto g=findGame(r);auto root=g.value("runtime_path","");require(folderSource(g)||(g.value("mounted",false)&&readOnly(root)),"Mount the source read-only first");uint64_t n=0,b=0;char error[1024]={};int rc=ampr_backend_measure(root.c_str(),&n,&b,error,sizeof(error));require(rc==0,error);return J{{"ok",true},{"logicalBytes",n},{"requiredOutputBytes",b}};}
    if(path=="/api/control/quit"){require(mutation,"Use POST to quit");require(get(r,"confirm")=="yes","Confirm quitting first");bool active;{std::lock_guard<std::mutex> guard(stateLock);active=state.value("active",false);}require(!active||get(r,"force")=="yes","A conversion is running; confirm cancelling it before quitting");
        // Exit after the response is sent; a running job is cancelled first so it can stop at a safe block boundary.
        startDetached([active](){if(active){abortJob=true;appendLog("Quit requested: cancelling the running job.\n");for(int n=0;n<600;n++){{std::lock_guard<std::mutex> guard(stateLock);if(!state.value("active",false))break;}std::this_thread::sleep_for(std::chrono::milliseconds(100));}}appendLog("Quit requested from the web interface. Exiting.\n");std::this_thread::sleep_for(std::chrono::milliseconds(500));_exit(0);});
        return J{{"ok",true},{"message",active?"Cancelling the conversion, then quitting":"Quitting"}};}
    std::lock_guard<std::mutex> operation(control);bool active;{std::lock_guard<std::mutex> guard(stateLock);active=state.value("active",false);}require(!active,"Wait until the current job finishes");
    if(path=="/api/gc/ampr-lz4/mount"){require(mutation,"Use POST to mount");require(!gameRunning(),"Close the running game before changing mounts");auto selected=findGame(r);if(folderSource(selected))return J{{"ok",true},{"message","Folder dumps need no mount"}};auto listing=shadow("games");for(auto& g:listing.at("games")){if(g.value("managed",false)&&g.value("mounted",false))shadow("games/unmount",J{{"title_id",g.at("title_id")}});}shadow("games/mount",J{{"title_id",selected.at("title_id")},{"mode","ro"}});return J{{"ok",true},{"message","ShadowMount request sent. Read-only verification follows."}};}
    if(path=="/api/gc/ampr-lz4/plan")return plan(r);
    if(path=="/api/gc/ampr-lz4/start"||path=="/api/gc/ampr-lz4/benchmark"){require(mutation,"Use POST to start");launch(r,path=="/api/gc/ampr-lz4/benchmark");return J{{"ok",true},{"message","Started"}};}
    if(path=="/api/gc/ampr-lz4/delete-partial"){require(mutation,"Use POST to delete partial output");char error[1024]={};int rc=ampr_backend_delete_partial(error,sizeof(error));require(rc==0,error);auto remaining=backendJson(ampr_backend_partial_info,32768);require(remaining.empty(),"Partial output remains; inspect diagnostics");return J{{"ok",true}};}
    if(path=="/api/gc/ampr-lz4/threads"){std::string input;if(mutation){J setting={{"packing",{{"readers",integer(r,"packReaders",2,6)},{"workers",integer(r,"packWorkers",3,6)},{"writers",integer(r,"packWriters",2,6)}}},{"loose",{{"workers",0},{"readers",integer(r,"looseReaders",3,6)},{"writers",integer(r,"looseWriters",4,6)}}},{"verification",{{"writers",0},{"readers",integer(r,"verifyReaders",3,6)},{"workers",integer(r,"verifyWorkers",4,6)}}}};input=setting.dump();}char output[8192]={};int rc=ampr_backend_thread_settings(mutation?input.c_str():nullptr,output,sizeof(output));require(rc==0,output);auto j=J::parse(output);j["ok"]=true;return j;}
    if(path=="/api/gc/ampr-lz4/benchmark/apply"){require(mutation,"Use POST to apply settings");char output[32768]={};int rc=ampr_backend_benchmark_apply(get(r,"id").c_str(),output,sizeof(output));require(rc==0,output);auto j=J::parse(output);j["ok"]=true;return j;}
    throw std::runtime_error("Unknown endpoint");
}
static void handle(int fd){Socket client(fd);timeout(fd,15);try{auto request=parse(receive(fd,16384));if(request.path=="/api/gc/ampr-lz4/log"){require(request.method=="GET","Use GET for logs");std::string tail;{std::lock_guard<std::mutex> guard(stateLock);tail=logTail;}respond(fd,200,tail,"text/plain; charset=utf-8");return;}if(request.path.rfind("/api/",0)==0){respond(fd,200,route(request).dump());return;}require(request.method=="GET","Use GET for assets");auto path=request.path=="/"?"/ampr-studio.html":request.path;for(auto& a:embeddedAssets){if(path=="/"+std::string(a.name)&&std::string(a.name)!="ampr-runtime.sprx"){respond(fd,200,std::string(reinterpret_cast<const char*>(a.data),a.size),a.mime);return;}}respond(fd,404,J{{"ok",false},{"error","Not found"}}.dump());}catch(const std::exception&e){try{respond(fd,400,J{{"ok",false},{"error",e.what()}}.dump());}catch(...){}}}
int main(){signal(SIGPIPE,SIG_IGN);try{
    fs::create_directories(fs::path(AMPR_STATE_ROOT)/"logs");for(auto& a:embeddedAssets){if(std::string(a.name)=="ampr-runtime.sprx"){auto path=fs::path(AMPR_STATE_ROOT)/"libSceAmpr.sprx";std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(a.data),a.size);f.flush();require(bool(f),"Cannot install AMPR runtime");require(chmod(path.c_str(),0644)==0,"Cannot set runtime permissions");}}
    Socket server(socket(AF_INET,SOCK_STREAM,0));require(server.fd>=0,"Cannot create service socket");int one=1;setsockopt(server.fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(STUDIO_PORT);address.sin_addr.s_addr=htonl(INADDR_ANY);require(bind(server.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Port is already in use");require(listen(server.fd,16)==0,"Cannot listen");std::mutex lock;std::condition_variable available;std::deque<int> pending;
    for(int n=0;n<4;n++)startDetached([&](){for(;;){int fd;{std::unique_lock<std::mutex> guard(lock);available.wait(guard,[&](){return !pending.empty();});fd=pending.front();pending.pop_front();}handle(fd);}},1024*1024);
    for(;;){int fd=accept(server.fd,nullptr,nullptr);if(fd<0){if(errno==EINTR)continue;throw std::runtime_error("Accept failed");}{std::lock_guard<std::mutex> guard(lock);if(pending.size()>=16){close(fd);continue;}pending.push_back(fd);}available.notify_one();}
}catch(const std::exception&e){fprintf(stderr,"LZ4 Compressor: %s\n",e.what());return 1;}}
