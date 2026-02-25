#include "sttnet.h"
#include <mysql/mysql.h>
#include <cctype>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <algorithm>
#include<sstream>
#include<vector>
#include<unordered_set>
using namespace std;
using namespace stt::file;
using namespace stt::network;
using namespace stt::system;
using namespace stt::data;
using namespace stt::time;

class SAConnection;
SAConnection* getConn();
void backPool(SAConnection *k);
void disconnectDB(SAConnection *con);

static constexpr int SA_MySQL_Client = 0;

class SAString
{
public:
    SAString()=default;
    SAString(const std::string &s):v(s){}
    SAString(const char *s):v(s?s:""){}
    SAString(const char *s,size_t n):v(s?std::string(s,n):std::string()){}
    const char* GetMultiByteChars() const { return v.c_str(); }
    const std::string& str() const { return v; }
private:
    std::string v;
};

class SAException: public std::exception
{
public:
    SAException(long code,const std::string &msg):native(code),text(msg){}
    long ErrNativeCode() const { return native; }
    SAString ErrText() const { return SAString(text); }
    const char* what() const noexcept override { return text.c_str(); }
private:
    long native=0;
    std::string text;
};

class SAConnection
{
public:
    SAConnection()=default;
    ~SAConnection(){ Disconnect(); }

    void Connect(const std::string &conn,const std::string &user,const std::string &pass,int)
    {
        Disconnect();
        std::string host="127.0.0.1";
        unsigned int port=3306;
        std::string db=conn;

        auto at=conn.find('@');
        std::string hp=conn;
        if(at!=std::string::npos)
        {
            hp=conn.substr(0,at);
            db=conn.substr(at+1);
        }
        auto comma=hp.find(',');
        if(comma!=std::string::npos)
        {
            host=hp.substr(0,comma);
            try{ port=static_cast<unsigned int>(std::stoul(hp.substr(comma+1))); }
            catch(...) { throw SAException(0,"invalid db port"); }
        }
        else if(!hp.empty())
            host=hp;

        db_=mysql_init(nullptr);
        if(!db_) throw SAException(0,"mysql_init failed");

        mysql_options(db_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

        if(!mysql_real_connect(db_,host.c_str(),user.c_str(),pass.c_str(),db.c_str(),port,nullptr,0))
        {
            std::string err=mysql_error(db_);
            long code=mysql_errno(db_);
            mysql_close(db_);
            db_=nullptr;
            throw SAException(code,err);
        }

        if(mysql_set_character_set(db_,"utf8mb4")!=0)
        {
            std::string err=mysql_error(db_);
            long code=mysql_errno(db_);
            mysql_close(db_);
            db_=nullptr;
            throw SAException(code,err);
        }

        connected_=true;
    }

    void Disconnect()
    {
        if(db_) mysql_close(db_);
        db_=nullptr;
        connected_=false;
    }

    bool isConnected() const { return connected_ && db_!=nullptr; }
    void setOption(const std::string&) {}
    MYSQL* raw() const { return db_; }

private:
    MYSQL *db_=nullptr;
    bool connected_=false;
};

struct SAParamValue
{
    enum Type{NONE,LONG_T,STRING_T} type=NONE;
    long long l=0;
    std::string s;
};

class SACommand;

class SAParamProxy
{
public:
    SAParamProxy(SACommand *owner,std::string key,SAParamValue::Type type):owner(owner),key(std::move(key)),type(type){}
    SAParamProxy& operator=(long v);
    SAParamProxy& operator=(int v){ return (*this)=static_cast<long>(v); }
    SAParamProxy& operator=(long long v){ return (*this)=static_cast<long>(v); }
    SAParamProxy& operator=(const std::string &v);
    SAParamProxy& operator=(const char *v);
private:
    SACommand *owner;
    std::string key;
    SAParamValue::Type type;
};

class SAParamAccessor
{
public:
    SAParamAccessor(SACommand *owner,std::string key):owner(owner),key(std::move(key)){}
    SAParamProxy setAsLong();
    SAParamProxy setAsString();
private:
    SACommand *owner;
    std::string key;
};

class SAFieldValue
{
public:
    SAFieldValue()=default;
    SAFieldValue(bool n,std::string v):is_null(n),val(std::move(v)){}
    SAString asString() const { return SAString(is_null?"":val); }
    long asLong() const
    {
        if(is_null || val.empty()) return 0;
        try{ return std::stol(val); }catch(...){ return 0; }
    }
    unsigned long asULong() const
    {
        if(is_null || val.empty()) return 0;
        try{ return std::stoul(val); }catch(...){ return 0; }
    }
private:
    bool is_null=true;
    std::string val;
};

class SACommand
{
public:
    SACommand(SAConnection *con,const std::string &sql):con_(con),sql_(sql){}
    ~SACommand(){ clearResult(); }

    SAParamAccessor Param(const std::string &k){ return SAParamAccessor(this,k); }
    SAParamAccessor Param(int k){ return SAParamAccessor(this,std::to_string(k)); }

    void setParam(const std::string &k,const SAParamValue &v){ params_[k]=v; }

    void Execute()
    {
        if(!con_ || !con_->raw()) throw SAException(0,"db not connected");
        clearResult();
        affected_=0;
        const std::string realSql=buildSQL();
        if(mysql_query(con_->raw(),realSql.c_str())!=0)
            throw SAException(mysql_errno(con_->raw()),mysql_error(con_->raw()));

        res_=mysql_store_result(con_->raw());
        if(!res_)
        {
            if(mysql_field_count(con_->raw())!=0)
                throw SAException(mysql_errno(con_->raw()),mysql_error(con_->raw()));
            affected_=static_cast<long long>(mysql_affected_rows(con_->raw()));
            return;
        }

        MYSQL_FIELD *fields=mysql_fetch_fields(res_);
        unsigned int n=mysql_num_fields(res_);
        fieldNames_.clear();
        nameToIndex_.clear();
        fieldNames_.reserve(n);
        for(unsigned int i=0;i<n;++i)
        {
            std::string fn=fields[i].name?fields[i].name:"";
            fieldNames_.push_back(fn);
            nameToIndex_[fn]=i;
        }
    }

    long RowsAffected() const
    {
        return static_cast<long>(affected_);
    }

    bool FetchNext()
    {
        if(!res_) return false;
        MYSQL_ROW row=mysql_fetch_row(res_);
        if(!row) return false;
        unsigned long *lens=mysql_fetch_lengths(res_);
        size_t n=fieldNames_.size();
        current_.assign(n,SAFieldValue());
        for(size_t i=0;i<n;++i)
        {
            if(!row[i]) current_[i]=SAFieldValue(true,"");
            else current_[i]=SAFieldValue(false,std::string(row[i],lens?lens[i]:std::strlen(row[i])));
        }
        return true;
    }

    SAFieldValue Field(int idx) const
    {
        if(idx<=0) return SAFieldValue();
        size_t i=static_cast<size_t>(idx-1);
        if(i>=current_.size()) return SAFieldValue();
        return current_[i];
    }

    SAFieldValue Field(const std::string &name) const
    {
        auto it=nameToIndex_.find(name);
        if(it==nameToIndex_.end()) return SAFieldValue();
        size_t i=it->second;
        if(i>=current_.size()) return SAFieldValue();
        return current_[i];
    }

private:
    friend class SAParamProxy;
    std::string escape(const std::string &s) const
    {
        std::string out;
        out.resize(s.size()*2+1);
        unsigned long n=mysql_real_escape_string(con_->raw(),out.data(),s.data(),s.size());
        out.resize(n);
        return out;
    }

    std::string buildSQL() const
    {
        std::string out;
        out.reserve(sql_.size()+64);
        bool inSingle=false;
        for(size_t i=0;i<sql_.size();++i)
        {
            char c=sql_[i];
            if(c=='\'' && (i==0 || sql_[i-1] != '\\'))
            {
                inSingle=!inSingle;
                out.push_back(c);
                continue;
            }
            if(!inSingle && c==':' && i+1<sql_.size())
            {
                size_t j=i+1;
                if(!(std::isalnum(static_cast<unsigned char>(sql_[j])) || sql_[j]=='_'))
                {
                    out.push_back(c);
                    continue;
                }
                while(j<sql_.size() && (std::isalnum(static_cast<unsigned char>(sql_[j])) || sql_[j]=='_')) ++j;
                std::string key=sql_.substr(i+1,j-(i+1));
                auto it=params_.find(key);
                if(it==params_.end())
                    throw SAException(0,"missing sql param: "+key);
                const SAParamValue &pv=it->second;
                if(pv.type==SAParamValue::LONG_T) out+=std::to_string(pv.l);
                else if(pv.type==SAParamValue::STRING_T) out+="'"+escape(pv.s)+"'";
                else out+="NULL";
                i=j-1;
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    void clearResult()
    {
        if(res_) mysql_free_result(res_);
        res_=nullptr;
        fieldNames_.clear();
        nameToIndex_.clear();
        current_.clear();
    }

    SAConnection *con_=nullptr;
    std::string sql_;
    std::unordered_map<std::string,SAParamValue> params_;
    MYSQL_RES *res_=nullptr;
    long long affected_=0;
    std::vector<std::string> fieldNames_;
    std::unordered_map<std::string,size_t> nameToIndex_;
    std::vector<SAFieldValue> current_;
};

inline SAParamProxy SAParamAccessor::setAsLong(){ return SAParamProxy(owner,key,SAParamValue::LONG_T); }
inline SAParamProxy SAParamAccessor::setAsString(){ return SAParamProxy(owner,key,SAParamValue::STRING_T); }

inline SAParamProxy& SAParamProxy::operator=(long v)
{
    SAParamValue pv;
    pv.type=type;
    pv.l=v;
    owner->setParam(key,pv);
    return *this;
}

inline SAParamProxy& SAParamProxy::operator=(const std::string &v)
{
    SAParamValue pv;
    pv.type=type;
    pv.s=v;
    owner->setParam(key,pv);
    return *this;
}

inline SAParamProxy& SAParamProxy::operator=(const char *v)
{
    return (*this)=std::string(v?v:"");
}

#define MAX_SIZE_UPLOAD 1024LL*1024*1024*2

LogFile* lf = nullptr;
HttpServer* httpserver = nullptr;
WebSocketServer* wsserver = nullptr;
static atomic<bool> g_shutdownRequested(false);

struct DBConfig
{
    string host="127.0.0.1";
    int port=3306;
    string database="im_db";
    string user="im";
    string password="";
    string charset="utf8mb4";
};

struct HttpRuntimeConfig
{
    unsigned long long maxFD=100000;
    int buffer_size_kb=1024*5;
    size_t finishQueue_cap=65536;
    bool security_open=true;
    int connectionNumLimit=10;
    int connectionSecs=1;
    int connectionTimes=3;
    int requestSecs=1;
    int requestTimes=20;
    int checkFrequency=30;
    int connectionTimeout=30;
    int listen_port=8080;
    int listen_threads=2;
    string cors_allow_origin="https://sttveil.pages.dev";
    string tls_cert="";
    string tls_key="";
    string tls_pwd="";
    string tls_ca="";
};

struct WsRuntimeConfig
{
    unsigned long long maxFD=50000;
    int buffer_size_kb=256;
    size_t finishQueue_cap=65536;
    bool security_open=true;
    int connectionNumLimit=20;
    int connectionSecs=5;
    int connectionTimes=30;
    int requestSecs=1;
    int requestTimes=10;
    int checkFrequency=60;
    int connectionTimeout=60*10;
    int listen_port=5050;
    int listen_threads=2;
    string tls_cert="";
    string tls_key="";
    string tls_pwd="";
    string tls_ca="";
};

DBConfig g_dbCfg;
HttpRuntimeConfig g_httpCfg;
WsRuntimeConfig g_wsCfg;

//用内存做一个通知消息模型，暂时不碰数据库
unordered_map<long,unordered_map<long,long>> pop_test;
unordered_map<long,unordered_map<long,long>> latest_test;

struct GroupInfo
{
    long id;
    string name;
    long owner_id;
    long key_epoch;
};
unordered_map<long,GroupInfo> idToGroup;
unordered_map<long,unordered_set<long>> groupMembers; // group_id -> members
unordered_map<long,unordered_set<long>> userGroups;   // user_id -> groups


//ws用户信息
struct userInf
{
    long fd;
    long id;
    string username;
    string token;
    string avatar_url;
    string kty;
    string crv;
    string spki;
};
userInf userinf[1000000]; //用户信息
unordered_map<string,long> tokenToIndex;//token->用户信息下标表
unordered_map<long,long> idtoIndex;//id->用户信息下标表

mutex conn_pool_mutex;
queue<SAConnection*> conn_pool;
atomic<int> connNum(0);


struct Inf
{
    long id;
    string username;
    string avatar_url;
    string kty;
    string crv;
    string spki;
};
//id->userInf(程序开始从数据库导入)
unordered_map<long,Inf> idToInf;

//username -> userInf (程序开始从数据库导入)
unordered_map<string,Inf> usernameToInf;

//userId -> 好友关系 (程序开始从数据库导入)
unordered_map<long,unordered_map<long,int>> idToRelationship; //0 无关系 1 好友

Inf getInf(const string &username)
{
    auto ii=usernameToInf.find(username);
    if(ii==usernameToInf.end())
        return Inf{};
    else
        return ii->second;
}


Inf getInf(const long &id)
{
    auto ii=idToInf.find(id);
    if(ii==idToInf.end())
        return Inf{};
    else
        return ii->second;
}

long getUserId(const string &username)
{
    auto ii=usernameToInf.find(username);
    if(ii==usernameToInf.end())
        return -1;
    else
        return ii->second.id;
}

int getRelationship(const long &id1,const long &id2)
{
    auto ii=idToRelationship.find(id1);
    if(ii==idToRelationship.end())
        return 0;
    else
    {
        auto jj=ii->second.find(id2);
        if(jj==ii->second.end())
            return 0;
        else
            return jj->second;
    }
}
void deleteUser(const string &username)
{
   auto ii=usernameToInf.find(username);
   if(ii==usernameToInf.end())
        return;
   else
   {
        auto jj=idToInf.find(ii->second.id);
        idToInf.erase(jj);
        usernameToInf.erase(ii);

   }
}
void changeUserAvatar(const string &username,const string &avatar_url)
{
    auto ii=usernameToInf.find(username);
   if(ii==usernameToInf.end())
        return;
   else
   {
        ii->second.avatar_url=avatar_url;
         auto jj=idToInf.find(ii->second.id);
         jj->second.avatar_url=avatar_url;
   }
}

void changeUsername(const string &oldName,const string &newName)
{
    auto ii=usernameToInf.find(oldName);
    if(ii==usernameToInf.end())
        return;
    else
    {
        usernameToInf[newName]=ii->second;
         auto jj=idToInf.find(ii->second.id);
         jj->second.username=newName;
    }
}
void deleteRelationship(const long &id1,const long &id2)
{
   auto ii=idToRelationship.find(id1);
   if(ii==idToRelationship.end())
        return;
    else
    {
        auto jj=ii->second.find(id2);
        if(jj==ii->second.end())
            return;
        else
            jj->second=0;
    }
    
    auto aa=idToRelationship.find(id2);
    if(aa==idToRelationship.end())
         return;
     else
     {
         auto bb=ii->second.find(id1);
         if(bb==ii->second.end())
             return;
         else
             bb->second=0;
     }
}
void addRelationship(const long &id1,const long &id2)
{
   //为id1
   auto ii=idToRelationship.find(id1);
   if(ii==idToRelationship.end())
   {
       idToRelationship.emplace(id1,unordered_map<long,int>{{id2,1}});
   }
   else
   {
       ii->second.insert_or_assign(id2,1);
   }
   //为id2
   auto jj=idToRelationship.find(id2);
   if(jj==idToRelationship.end())
   {
       idToRelationship.emplace(id2,unordered_map<long,int>{{id1,1}});
   }
   else
   {
       jj->second.insert_or_assign(id1,1);
   }
}
void addUser(const string &username,const long &id,const string &avatar_url,const string &kty,const string &crv,const string &spki)
{
   usernameToInf.insert_or_assign(username,Inf{id,username,avatar_url,kty,crv,spki});
    idToInf.insert_or_assign(id,Inf{id,username,avatar_url,kty,crv,spki});
}

static string trimCopy(const string &s)
{
    size_t l=0,r=s.size();
    while(l<r && (s[l]==' '||s[l]=='\t'||s[l]=='\r'||s[l]=='\n')) l++;
    while(r>l && (s[r-1]==' '||s[r-1]=='\t'||s[r-1]=='\r'||s[r-1]=='\n')) r--;
    return s.substr(l,r-l);
}

static vector<string> splitCSV(const string &s)
{
    vector<string> out;
    string cur;
    for(char c:s)
    {
        if(c==',')
        {
            string t=trimCopy(cur);
            if(!t.empty()) out.push_back(t);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    string t=trimCopy(cur);
    if(!t.empty()) out.push_back(t);
    return out;
}

static bool parseBoolCfg(const string &v,bool defVal)
{
    string t=trimCopy(v);
    std::transform(t.begin(),t.end(),t.begin(),[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if(t=="1"||t=="true"||t=="yes"||t=="on") return true;
    if(t=="0"||t=="false"||t=="no"||t=="off") return false;
    return defVal;
}

static int parseIntCfg(const string &v,int defVal)
{
    try{ return stoi(trimCopy(v)); }catch(...){ return defVal; }
}

static unsigned long long parseULLCfg(const string &v,unsigned long long defVal)
{
    try{ return stoull(trimCopy(v)); }catch(...){ return defVal; }
}

static size_t parseSizeCfg(const string &v,size_t defVal)
{
    try{ return static_cast<size_t>(stoull(trimCopy(v))); }catch(...){ return defVal; }
}

static bool loadKvConfig(const string &path,unordered_map<string,string> &out)
{
    ifstream fin(path);
    if(!fin.is_open()) return false;
    string line;
    while(getline(fin,line))
    {
        string t=trimCopy(line);
        if(t.empty() || t[0]=='#' || t[0]==';') continue;
        auto pos=t.find('=');
        if(pos==string::npos) continue;
        string k=trimCopy(t.substr(0,pos));
        string v=trimCopy(t.substr(pos+1));
        if(!k.empty()) out[k]=v;
    }
    return true;
}

static void loadRuntimeConfig()
{
    unordered_map<string,string> dbkv;
    unordered_map<string,string> svkv;
    bool dbOk=loadKvConfig("./config/database.conf",dbkv);
    bool svOk=loadKvConfig("./config/service.conf",svkv);
    if(!dbOk) lf->writeLog("配置提示: 未读取到 ./config/database.conf，使用内置非敏感默认配置（密码默认空）");
    if(!svOk) lf->writeLog("配置提示: 未读取到 ./config/service.conf，使用内置非敏感默认配置（TLS 默认空）");

    if(dbkv.count("db.host")) g_dbCfg.host=dbkv["db.host"];
    if(dbkv.count("db.port")) g_dbCfg.port=parseIntCfg(dbkv["db.port"],g_dbCfg.port);
    if(dbkv.count("db.database")) g_dbCfg.database=dbkv["db.database"];
    if(dbkv.count("db.user")) g_dbCfg.user=dbkv["db.user"];
    if(dbkv.count("db.password")) g_dbCfg.password=dbkv["db.password"];
    if(dbkv.count("db.charset")) g_dbCfg.charset=dbkv["db.charset"];

    if(svkv.count("http.max_fd")) g_httpCfg.maxFD=parseULLCfg(svkv["http.max_fd"],g_httpCfg.maxFD);
    if(svkv.count("http.buffer_size_kb")) g_httpCfg.buffer_size_kb=parseIntCfg(svkv["http.buffer_size_kb"],g_httpCfg.buffer_size_kb);
    if(svkv.count("http.finish_queue_cap")) g_httpCfg.finishQueue_cap=parseSizeCfg(svkv["http.finish_queue_cap"],g_httpCfg.finishQueue_cap);
    if(svkv.count("http.security_open")) g_httpCfg.security_open=parseBoolCfg(svkv["http.security_open"],g_httpCfg.security_open);
    if(svkv.count("http.connection_num_limit")) g_httpCfg.connectionNumLimit=parseIntCfg(svkv["http.connection_num_limit"],g_httpCfg.connectionNumLimit);
    if(svkv.count("http.connection_secs")) g_httpCfg.connectionSecs=parseIntCfg(svkv["http.connection_secs"],g_httpCfg.connectionSecs);
    if(svkv.count("http.connection_times")) g_httpCfg.connectionTimes=parseIntCfg(svkv["http.connection_times"],g_httpCfg.connectionTimes);
    if(svkv.count("http.request_secs")) g_httpCfg.requestSecs=parseIntCfg(svkv["http.request_secs"],g_httpCfg.requestSecs);
    if(svkv.count("http.request_times")) g_httpCfg.requestTimes=parseIntCfg(svkv["http.request_times"],g_httpCfg.requestTimes);
    if(svkv.count("http.check_frequency")) g_httpCfg.checkFrequency=parseIntCfg(svkv["http.check_frequency"],g_httpCfg.checkFrequency);
    if(svkv.count("http.connection_timeout")) g_httpCfg.connectionTimeout=parseIntCfg(svkv["http.connection_timeout"],g_httpCfg.connectionTimeout);
    if(svkv.count("http.listen_port")) g_httpCfg.listen_port=parseIntCfg(svkv["http.listen_port"],g_httpCfg.listen_port);
    if(svkv.count("http.listen_threads")) g_httpCfg.listen_threads=parseIntCfg(svkv["http.listen_threads"],g_httpCfg.listen_threads);
    if(svkv.count("http.cors_allow_origin")) g_httpCfg.cors_allow_origin=trimCopy(svkv["http.cors_allow_origin"]);
    if(svkv.count("http.tls_cert")) g_httpCfg.tls_cert=svkv["http.tls_cert"];
    if(svkv.count("http.tls_key")) g_httpCfg.tls_key=svkv["http.tls_key"];
    if(svkv.count("http.tls_password")) g_httpCfg.tls_pwd=svkv["http.tls_password"];
    if(svkv.count("http.tls_ca")) g_httpCfg.tls_ca=svkv["http.tls_ca"];

    if(svkv.count("ws.max_fd")) g_wsCfg.maxFD=parseULLCfg(svkv["ws.max_fd"],g_wsCfg.maxFD);
    if(svkv.count("ws.buffer_size_kb")) g_wsCfg.buffer_size_kb=parseIntCfg(svkv["ws.buffer_size_kb"],g_wsCfg.buffer_size_kb);
    if(svkv.count("ws.finish_queue_cap")) g_wsCfg.finishQueue_cap=parseSizeCfg(svkv["ws.finish_queue_cap"],g_wsCfg.finishQueue_cap);
    if(svkv.count("ws.security_open")) g_wsCfg.security_open=parseBoolCfg(svkv["ws.security_open"],g_wsCfg.security_open);
    if(svkv.count("ws.connection_num_limit")) g_wsCfg.connectionNumLimit=parseIntCfg(svkv["ws.connection_num_limit"],g_wsCfg.connectionNumLimit);
    if(svkv.count("ws.connection_secs")) g_wsCfg.connectionSecs=parseIntCfg(svkv["ws.connection_secs"],g_wsCfg.connectionSecs);
    if(svkv.count("ws.connection_times")) g_wsCfg.connectionTimes=parseIntCfg(svkv["ws.connection_times"],g_wsCfg.connectionTimes);
    if(svkv.count("ws.request_secs")) g_wsCfg.requestSecs=parseIntCfg(svkv["ws.request_secs"],g_wsCfg.requestSecs);
    if(svkv.count("ws.request_times")) g_wsCfg.requestTimes=parseIntCfg(svkv["ws.request_times"],g_wsCfg.requestTimes);
    if(svkv.count("ws.check_frequency")) g_wsCfg.checkFrequency=parseIntCfg(svkv["ws.check_frequency"],g_wsCfg.checkFrequency);
    if(svkv.count("ws.connection_timeout")) g_wsCfg.connectionTimeout=parseIntCfg(svkv["ws.connection_timeout"],g_wsCfg.connectionTimeout);
    if(svkv.count("ws.listen_port")) g_wsCfg.listen_port=parseIntCfg(svkv["ws.listen_port"],g_wsCfg.listen_port);
    if(svkv.count("ws.listen_threads")) g_wsCfg.listen_threads=parseIntCfg(svkv["ws.listen_threads"],g_wsCfg.listen_threads);
    if(svkv.count("ws.tls_cert")) g_wsCfg.tls_cert=svkv["ws.tls_cert"];
    if(svkv.count("ws.tls_key")) g_wsCfg.tls_key=svkv["ws.tls_key"];
    if(svkv.count("ws.tls_password")) g_wsCfg.tls_pwd=svkv["ws.tls_password"];
    if(svkv.count("ws.tls_ca")) g_wsCfg.tls_ca=svkv["ws.tls_ca"];

    lf->writeLog("配置加载完成: db="+g_dbCfg.host+":"+to_string(g_dbCfg.port)+"/"+g_dbCfg.database
        +", http_port="+to_string(g_httpCfg.listen_port)
        +", ws_port="+to_string(g_wsCfg.listen_port));
}

// Extract raw JSON object assigned to a key from a larger JSON text.
// Example: key=payload in {"payload":{"kind":"text","text":"..."}}
static bool extractJsonObjectField(const string &json,const string &key,string &out)
{
    string tag="\"" + key + "\"";
    size_t kp=json.find(tag);
    if(kp==string::npos) return false;
    size_t p=json.find(':',kp+tag.size());
    if(p==string::npos) return false;
    ++p;
    while(p<json.size() && (json[p]==' '||json[p]=='\t'||json[p]=='\r'||json[p]=='\n')) ++p;
    if(p>=json.size() || json[p]!='{') return false;
    size_t start=p;
    int depth=0;
    bool inStr=false,esc=false;
    for(size_t i=p;i<json.size();++i)
    {
        char ch=json[i];
        if(inStr)
        {
            if(esc) esc=false;
            else if(ch=='\\') esc=true;
            else if(ch=='"') inStr=false;
            continue;
        }
        if(ch=='"') { inStr=true; continue; }
        if(ch=='{') depth++;
        else if(ch=='}')
        {
            depth--;
            if(depth==0)
            {
                out=json.substr(start,i-start+1);
                return true;
            }
        }
    }
    return false;
}

bool isGroupMember(const long &group_id,const long &user_id)
{
    auto gi=groupMembers.find(group_id);
    if(gi!=groupMembers.end() && gi->second.find(user_id)!=gi->second.end())
        return true;

    // fallback: 服务重启后内存缓存未命中时，直接查库并回填缓存
    SAConnection *con=getConn();
    if(con==nullptr) return false;
    try
    {
        SACommand cmd(con,
        "SELECT 1 FROM t_group_member WHERE group_id=:g AND user_id=:u LIMIT 1");
        cmd.Param("g").setAsLong()=group_id;
        cmd.Param("u").setAsLong()=user_id;
        cmd.Execute();
        bool ok=cmd.FetchNext();
        backPool(con);
        if(ok)
        {
            groupMembers[group_id].insert(user_id);
            userGroups[user_id].insert(group_id);
        }
        return ok;
    }
    catch(...)
    {
        disconnectDB(con);
        return false;
    }
}

void addGroupMemberMem(const long &group_id,const long &user_id)
{
    groupMembers[group_id].insert(user_id);
    userGroups[user_id].insert(group_id);
}

long getGroupEpoch(const long &group_id)
{
    auto gi=idToGroup.find(group_id);
    if(gi==idToGroup.end()) return 1;
    return gi->second.key_epoch<=0 ? 1 : gi->second.key_epoch;
}

void notifyGroupEpochChanged(const long &group_id,const unordered_set<long> &extraNotify={})
{
    auto gi=idToGroup.find(group_id);
    if(gi==idToGroup.end()) return;
    long epoch=getGroupEpoch(group_id);
    string msg=JsonHelper::createJson("type","group_key_epoch","group_id",group_id,"key_epoch",epoch);
    unordered_set<long> targets=extraNotify;
    auto mi=groupMembers.find(group_id);
    if(mi!=groupMembers.end())
    {
        for(auto uid:mi->second) targets.insert(uid);
    }
    for(auto uid:targets)
    {
        auto oi=idtoIndex.find(uid);
        if(oi!=idtoIndex.end())
        {
            wsserver->sendMessage(userinf[oi->second].fd,msg);
            wsserver->sendMessage(userinf[oi->second].fd,"{ \"type\": \"groups_refresh\" }");
        }
    }
}

bool bumpGroupEpochDBAndMem(const long &group_id,string &err)
{
    SAConnection *con=getConn();
    if(con==nullptr)
    {
        err="服务器错误";
        return false;
    }
    try
    {
        SACommand upd(con,"UPDATE t_group SET key_epoch=key_epoch+1 WHERE id=:g");
        upd.Param("g").setAsLong()=group_id;
        upd.Execute();
        SACommand sel(con,"SELECT key_epoch FROM t_group WHERE id=:g");
        sel.Param("g").setAsLong()=group_id;
        sel.Execute();
        long epoch=1;
        if(sel.FetchNext()) epoch=sel.Field("key_epoch").asLong();
        backPool(con);
        auto gi=idToGroup.find(group_id);
        if(gi!=idToGroup.end()) gi->second.key_epoch=(epoch<=0?1:epoch);
        return true;
    }
    catch(SAException &e)
    {
        err=e.ErrText().GetMultiByteChars();
        disconnectDB(con);
        return false;
    }
}

// 返回值：
// -2 太大（>1MB）
// -1 非 PNG / JPEG / WEBP
//  0 PNG
//  1 JPEG
//  2 WEBP
int detectImage(const string &data)
{
    const size_t MAX_SIZE = 1024 * 1024; // 1MB

    // 1) 大小限制
    if (data.size() > MAX_SIZE)
        return -2;

    // 至少要能放下最短的 magic
    if (data.size() < 12)
        return -1;

    const uint8_t *b = reinterpret_cast<const uint8_t*>(data.data());

    // 2) PNG: 89 50 4E 47 0D 0A 1A 0A
    if (data.size() >= 8 &&
        b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47 &&
        b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A)
    {
        return 0;
    }

    // 3) JPEG: FF D8 FF
    if (data.size() >= 3 &&
        b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    {
        return 1;
    }

    // 4) WEBP: RIFF xxxx WEBP
    // 52 49 46 46 ???? 57 45 42 50
    if (data.size() >= 12 &&
        b[0] == 0x52 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x46 &&
        b[8] == 0x57 && b[9] == 0x45 && b[10] == 0x42 && b[11] == 0x50)
    {
        return 2;
    }

    return -1;
}
std::string hexDecode(const std::string& hex)
{
    auto hexVal = [](char c)->int {
        if(c>='0' && c<='9') return c-'0';
        if(c>='a' && c<='f') return c-'a'+10;
        if(c>='A' && c<='F') return c-'A'+10;
        return -1;
    };

    if(hex.empty() || (hex.length() % 2) != 0) return "";

    std::string output;
    output.reserve(hex.length() / 2);
    for(size_t i=0;i<hex.length();i+=2)
    {
        int hi=hexVal(hex[i]);
        int lo=hexVal(hex[i+1]);
        if(hi<0 || lo<0) return "";
        output.push_back(static_cast<char>((hi<<4)|lo));
    }
    return output;
}


static bool looksLikeHexText(const std::string &s)
{
    if(s.empty() || (s.size()%2)!=0) return false;
    for(char c:s)
    {
        bool ok=(c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
        if(!ok) return false;
    }
    return true;
}

static bool looksLikeJsonContainer(const std::string &s)
{
    string t=trimCopy(s);
    if(t.size()<2) return false;
    if(t.front()=='{' && t.back()=='}') return true;
    if(t.front()=='[' && t.back()==']') return true;
    return false;
}

static std::string decodePayloadJsonStored(const std::string &stored)
{
    string t=trimCopy(stored);
    if(t.empty()) return "";
    if(looksLikeJsonContainer(t)) return t;
    if(looksLikeHexText(t))
    {
        string d=hexDecode(t);
        d=trimCopy(d);
        if(looksLikeJsonContainer(d)) return d;
    }
    return "";
}

bool connectDB(SAConnection *con)
{
        try
        {
            string dbConn=g_dbCfg.host+","+to_string(g_dbCfg.port)+"@"+g_dbCfg.database;
            con->Connect(
            dbConn, 
            g_dbCfg.user,
            g_dbCfg.password,
            SA_MySQL_Client
        );
		if(con->isConnected())
        {
            connNum++;
            lf->writeLog("创建数据库新连接成功 目前连接数量= "+to_string(connNum));
            //con->setAutoCommit(SA_AutoCommitOff);
            SACommand cmd(con, "SET NAMES "+g_dbCfg.charset);
            cmd.Execute();
            con->setOption("ClientCharset="+g_dbCfg.charset);
            return true;
        }
        else
        {
            lf->writeLog("创建数据库新连接失败");
            con->Disconnect();
            return false;
        }
        }
        catch(SAException &ex)
        {
            lf->writeLog(string("连接数据库失败 error: ")+ex.ErrText().GetMultiByteChars());
            return false;
        }
}

static MYSQL* connectDBNative(string &err)
{
    MYSQL *conn=mysql_init(nullptr);
    if(!conn)
    {
        err="mysql_init failed";
        return nullptr;
    }
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, g_dbCfg.charset.c_str());
    if(!mysql_real_connect(conn,g_dbCfg.host.c_str(),g_dbCfg.user.c_str(),g_dbCfg.password.c_str(),g_dbCfg.database.c_str(),(unsigned int)g_dbCfg.port,nullptr,0))
    {
        err=mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }
    if(mysql_set_character_set(conn,g_dbCfg.charset.c_str())!=0)
    {
        err=mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }
    if(mysql_query(conn,("SET NAMES "+g_dbCfg.charset).c_str())!=0)
    {
        err=mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }
    return conn;
}
void disconnectDB(SAConnection *con)
{
    con->Disconnect();
    connNum--;
    lf->writeLog("数据库断开连接 目前连接数量= "+to_string(connNum));
}
SAConnection* getConn()
{
    unique_lock<mutex> lock(conn_pool_mutex);
    if(!conn_pool.empty())
    {
        SAConnection *k=conn_pool.front();
        conn_pool.pop();
        return k;
    }
    else
    {
        SAConnection *k=new SAConnection;
        if(connectDB(k))
            return k;
        else
        {
            delete k;
            k=nullptr;
            return k;
        }
    }
}
void backPool(SAConnection *k)
{
    unique_lock<mutex> lock(conn_pool_mutex);
    conn_pool.push(k);
}
void loadData()//从数据库导入数据
{
    lf->writeLog("导入数据开始...");
    //导入名字->id表
    //数据库
    SAConnection *con;
                    while(1)
                    {
                        con=getConn();
                        if(con==nullptr)
                        {
                            lf->writeLog("导入数据失败，没能获取到数据库连接池的连接");
                            sleep(1);
                        }
                        else
                        {
                            break;
                        }
                    }
                    try
                    {
                        SACommand ensureGroup(con,
                        "CREATE TABLE IF NOT EXISTS t_group ("
                        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                        "name VARCHAR(128) NOT NULL,"
                        "owner_id BIGINT UNSIGNED NOT NULL,"
                        "key_epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,"
                        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
                        ")");
                        ensureGroup.Execute();
                        SACommand ensureGroupEpoch(con,
                        "ALTER TABLE t_group ADD COLUMN key_epoch BIGINT UNSIGNED NOT NULL DEFAULT 1");
                        try{ ensureGroupEpoch.Execute(); }catch(...){}

                        SACommand ensureGroupMember(con,
                        "CREATE TABLE IF NOT EXISTS t_group_member ("
                        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                        "group_id BIGINT UNSIGNED NOT NULL,"
                        "user_id BIGINT UNSIGNED NOT NULL,"
                        "role ENUM('owner','member') NOT NULL DEFAULT 'member',"
                        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                        "UNIQUE KEY uk_group_user(group_id,user_id)"
                        ")");
                        ensureGroupMember.Execute();

                        SACommand ensureGroupMsg(con,
                        "CREATE TABLE IF NOT EXISTS t_group_message ("
                        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                        "group_id BIGINT UNSIGNED NOT NULL,"
                        "from_id BIGINT UNSIGNED NOT NULL,"
                        "payload_json LONGTEXT NOT NULL,"
                        "ts BIGINT UNSIGNED NOT NULL,"
                        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
                        ")");
                        ensureGroupMsg.Execute();

                        //获取username和id
                        SACommand cmd(con,
                        "SELECT username,id,avatar_url,pubkey_alg,pubkey_curve,pubkey_spki FROM t_user");
                        cmd.Execute();

                    while (cmd.FetchNext())
                    {
                        long id = cmd.Field(2).asLong();
                        string username=cmd.Field(1).asString().GetMultiByteChars();
                        string avatar_url=cmd.Field(3).asString().GetMultiByteChars();
                        string pubkey_alg=cmd.Field(4).asString().GetMultiByteChars();
                        string pubkey_curve=cmd.Field(5).asString().GetMultiByteChars();
                        string pubkey_spki=cmd.Field(6).asString().GetMultiByteChars();
                        usernameToInf.emplace(username,Inf{id,username,avatar_url,pubkey_alg,pubkey_curve,pubkey_spki});
                        idToInf.emplace(id,Inf{id,username,avatar_url,pubkey_alg,pubkey_curve,pubkey_spki});
                    }

                    

                

                    
                    //获取好友关系
                    SACommand cmdd(con,
                    "SELECT user_id,peer_id FROM t_friend WHERE status=:c");
                    
                    cmdd.Param("c").setAsString() = "accepted";
                    
                    cmdd.Execute();

                    while(cmdd.FetchNext())
                    {
                        long id1 = cmdd.Field(1).asLong();
                        long id2 = cmdd.Field(2).asLong();
                        //为id1
                        auto ii=idToRelationship.find(id1);
                        if(ii==idToRelationship.end())
                        {
                            idToRelationship.emplace(id1,unordered_map<long,int>{{id2,1}});
                        }
                        else
                        {
                            ii->second.emplace(id2,1);
                        }
                        //为id2
                        auto jj=idToRelationship.find(id2);
                        if(jj==idToRelationship.end())
                        {
                            idToRelationship.emplace(id2,unordered_map<long,int>{{id1,1}});
                        }
                        else
                        {
                            jj->second.emplace(id1,1);
                        }
                    }

                    SACommand cg(con,
                    "SELECT id,name,owner_id,key_epoch FROM t_group");
                    cg.Execute();
                    while(cg.FetchNext())
                    {
                        long gid=cg.Field("id").asLong();
                        string gname=cg.Field("name").asString().GetMultiByteChars();
                        long owner=cg.Field("owner_id").asLong();
                        long epoch=cg.Field("key_epoch").asLong();
                        if(epoch<=0) epoch=1;
                        idToGroup.insert_or_assign(gid,GroupInfo{gid,gname,owner,epoch});
                    }

                    SACommand cgm(con,
                    "SELECT group_id,user_id FROM t_group_member");
                    cgm.Execute();
                    while(cgm.FetchNext())
                    {
                        long gid=cgm.Field("group_id").asLong();
                        long uid=cgm.Field("user_id").asLong();
                        addGroupMemberMem(gid,uid);
                    }

                    //获取最后的消息
                    SACommand cmddd(con,
                    "SELECT from_id,to_id,MAX(ts) AS max_ts FROM t_message GROUP BY from_id, to_id");
                    
                    
                    cmddd.Execute();

                    while(cmddd.FetchNext())
                    {
                        long id1 = cmddd.Field(1).asLong();
                        long id2 = cmddd.Field(2).asLong();
                        long ts=cmddd.Field(3).asLong();
                        //入表
                        auto ii=latest_test.find(id1);
                        if(ii==latest_test.end())
                        {
                            latest_test.emplace(id1,unordered_map<long,long>{{id2,ts}});
                        }
                        else
                        {
                            ii->second.emplace(id2,ts);
                        }
                        
                    }

                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("loadData: 数据库连接异常，结束本次导入并继续启动服务。");
                            if(con) disconnectDB(con);
                            return;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("loadData: SQL 错误 msg="+string(e.ErrText().GetMultiByteChars())+"，结束本次导入并继续启动服务。");
                        if(con) disconnectDB(con);
                        return;
                        
                    }
    //导入id->好友关系表
    lf->writeLog("导入数据结束...开始执行服务程序...");
}
int main(int argc, char* argv[])
{   
    
    lf = new LogFile();
    ServerSetting::init(lf, "Chinese");
    loadRuntimeConfig();
    if(g_dbCfg.host.empty() || g_dbCfg.database.empty() || g_dbCfg.user.empty())
    {
        lf->writeLog("启动失败: 数据库配置不完整，请检查 host/database/user");
        return 1;
    }
    if(g_dbCfg.password.empty())
    {
        lf->writeLog("启动失败: 数据库密码为空，请在 config/database.conf 设置 db.password");
        return 1;
    }
    if(g_httpCfg.cors_allow_origin.empty())
    {
        lf->writeLog("启动失败: CORS 前端地址为空，请设置 http.cors_allow_origin");
        return 1;
    }
    if(g_httpCfg.tls_cert.empty() || g_httpCfg.tls_key.empty())
    {
        lf->writeLog("启动失败: HTTP TLS 证书或私钥为空，已拒绝启动（仅允许 HTTPS）");
        return 1;
    }
    if(g_wsCfg.tls_cert.empty() || g_wsCfg.tls_key.empty())
    {
        lf->writeLog("启动失败: WebSocket TLS 证书或私钥为空，已拒绝启动（仅允许 WSS）");
        return 1;
    }
    loadData();
    httpserver = new HttpServer(
        g_httpCfg.maxFD,
        g_httpCfg.buffer_size_kb,
        g_httpCfg.finishQueue_cap,
        g_httpCfg.security_open,
        g_httpCfg.connectionNumLimit,
        g_httpCfg.connectionSecs,
        g_httpCfg.connectionTimes,
        g_httpCfg.requestSecs,
        g_httpCfg.requestTimes,
        g_httpCfg.checkFrequency,
        g_httpCfg.connectionTimeout
    );
    httpserver->setTLS(
        g_httpCfg.tls_cert.c_str(),
        g_httpCfg.tls_key.c_str(),
        g_httpCfg.tls_pwd.c_str(),
        g_httpCfg.tls_ca.c_str()
    );
    /*
     * HTTP：从请求中提取 key（用于路由/上下文）
     */
    httpserver->setGetKeyFunction(
        [](HttpServerFDHandler&, HttpRequestInformation& inf) -> int {
            inf.ctx["key"] = inf.loc;  // use URL as key
            return 1;
        }
    );

    httpserver->setGlobalSolveFunction(
        [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
            if(inf.loc.find("/resources/avatars/")!=string::npos)
            {
                httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //读取文件数据
                    File f;
                    if(!f.openFile("STTChat_Resources/"+inf.loc.substr(1),false))
                    {
                        inf.ctx["body"]=string("");
                        inf.ctx["header"]=string("");
                        inf.ctx["code"]=string("404 not found");
                        return 1;
                    }
                    string data;
                    f.readAll(data);
                    
                    inf.ctx["body"]=move(data);
                    inf.ctx["header"]=move(HttpStringUtil::createHeader("Content-Type","image/png","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400","Cache-Control","public, max-age=3600"));
                    inf.ctx["code"]=string("200");
                    return 1;
                },
                k,
                inf
                );
                return 0;
            }
            else if(inf.loc.find("/resources/files/")!=string::npos)
            {
                if (inf.type == "OPTIONS")
                {
                    if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                        return 1;
                    else
                        return -2;
            
                }
                else
                {
                httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //读取文件数据
                    File f;
                    if(!f.openFile("STTChat_Resources/"+inf.loc.substr(1),false))
                    {
                        inf.ctx["body"]=string("");
                        inf.ctx["header"]=string("");
                        inf.ctx["code"]=string("404 not found");
                        return 1;
                    }
                    string data;
                    f.readAll(data);
                    
                    inf.ctx["body"]=move(data);
                    inf.ctx["header"]=move(HttpStringUtil::createHeader("Content-Type","application/octet-stream","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400","Cache-Control","public, max-age=3600"));
                    inf.ctx["code"]=string("200");
                    return 1;
                
                },
                k,
                inf
                );
                return 0;
                }
            }
            else
            {
                inf.ctx["body"]=string("");
                inf.ctx["header"]=string("");
                inf.ctx["code"]=string("404 not found");
                return 1;
            }
            
        }
    );
    httpserver->setGlobalSolveFunction(
        [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                if(inf.type != "OPTIONS")
                {
                    if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                            return -2;
                    return 1;
                }
                return 1;
        }            
    );

    httpserver->setFunction(
        "/ping",
        [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
            k.sendBack("pong");
            return 1;
        }
    );
    httpserver->setFunction(
        "/async",
        [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["body"]=string("async pong");
                    inf.ctx["header"]=string("");
                    inf.ctx["code"]=string("200");
                    return 1;
                },
                k,
                inf
            );
            return 0;  // handled asynchronously
        }
    );  

    httpserver->setFunction(
        "/async",
        [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
            if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                return -2;
            return 1;
        }
    );
    httpserver->setFunction("/api/files/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    inf.ctx["token"]=move(token);                    
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else 
            return -2;
    });
    httpserver->setFunction("/api/files/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :上传文件失败，token长度不合法或者没有带上头像文件");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :上传文件失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/files/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    //检查头像数据
                    if(inf.body.length()>MAX_SIZE_UPLOAD)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :上传文件失败，头像太大(>2G)");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","上传文件失败，头像太大(>2G)"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                   
                    
                    //存入本地
                    string td;
                    string rdm;
                    RandomUtil::getRandomStr_base64(rdm,128);
                    DateTime::getTime(td,"yyyy-mm-dd-hh-mi-ss-sss");
                    string filepath="STTChat_Resources/resources/files/"+rdm+td;
                    File f;
                    bool ok=
                    f.openFile(filepath,true,1,MAX_SIZE_UPLOAD) &&
                    f.lockMemory() &&
                    f.formatC() &&
                    f.writeC(inf.body.c_str(),0,inf.body.length()) &&
                    f.unlockMemory();
                    if(!ok)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :上传文件失败保存文件在本地失败");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","上传文件失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    ///resources/files
                    auto pos=filepath.find("/resources/files");
                    filepath=filepath.substr(pos,filepath.length()-pos);
                    inf.ctx["body"]=JsonHelper::createJson("success",true,"file_url",filepath);
                    inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                    inf.ctx["code"]=string("200");
                    inf.ctx["close"]=false;
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/files/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/avatar/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    inf.ctx["token"]=move(token);                    
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else 
            return -2;
    });
    httpserver->setFunction("/api/avatar/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :换头像失败，token长度不合法或者没有带上头像文件");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :换头像失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/avatar/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["ok"]=false;
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    //检查头像数据
                    int ret=detectImage(inf.body);
                    if(ret==-2)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，头像太大(>1mb)");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，头像太大(>1mb)"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    else if(ret ==-1)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，头像类型不合法(必须为png/jpeg/webp)");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，头像类型不合法(必须为png/jpeg/webp),请勿做一些肮脏的事情"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=true;
                        return 1;
                    }
                    
                    //存入本地
                    string filepath;
                    if(ret==0)
                        filepath="/resources/avatars/"+userinf[userinf_fd].username+".png";
                    else if(ret==1)
                        filepath="/resources/avatars/"+userinf[userinf_fd].username+".jpeg";
                    else if(ret==2)
                        filepath="/resources/avatars/"+userinf[userinf_fd].username+".webp";
                    File f;
                    bool ok=
                    f.openFile("STTChat_Resources"+filepath,true,1,1024*1024) &&
                    f.lockMemory() &&
                    f.formatC() &&
                    f.writeC(inf.body.c_str(),0,inf.body.length()) &&
                    f.unlockMemory();
                    if(!ok)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，保存文件在本地失败");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    //把路径存入数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd(con,
                        "UPDATE t_user "
                        "SET avatar_url = :path "
                        "WHERE id = :id"
                    );
                   
                    cmd.Param("path").setAsString() =filepath.c_str();
                    cmd.Param("id").setAsLong()=userinf[userinf_fd].id;
                    cmd.Execute();
                    //long rows = cmd.RowsAffected();

                    //if (rows != 1)
                    //{
                    //    lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，没有行影响 id="+to_string(userinf[userinf_fd].id));
                    //    backPool(con);
                    //    string res;
                    //    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，服务器错误"),res);
                    //    if(k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400")))
                    //        return 1;
                    //    else
                    //        return -2;
                    //}
                    
                    lf->writeLog("username="+userinf[userinf_fd].username+" :换头像成功 新头像位置:"+filepath);
                    backPool(con);
                    //发回json
                    
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"avatar_url",filepath),res);
                    inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        inf.ctx["ok"]=true;
                        inf.ctx["filepath"]=move(filepath);
                        inf.ctx["username"]=userinf[userinf_fd].username;
                        //inf.ctx["userinf_fd"]=move(userinf_fd);
                        return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/avatar/upload",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<bool>(inf.ctx["ok"]))
            {
                changeUserAvatar(any_cast<string>(inf.ctx["username"]),any_cast<string>(inf.ctx["filepath"]));

                userinf[any_cast<long>(inf.ctx["userinf_fd"])].avatar_url=any_cast<string>(inf.ctx["filepath"]);

            }
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/user/profile",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="PATCH")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    //解析json
                    string username="";
                    JsonHelper::getValue(inf.body,username,"value","display_name");
                    
                    inf.ctx["username"]=move(username);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else
            return -2;
    });
    httpserver->setFunction("/api/user/profile",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "PATCH")
        {
                    if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :改名失败，token长度不合法或没带上请求体");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
                    //解析json
                    if(any_cast<string>(inf.ctx["username"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :改名失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :改名失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/user/profile",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "PATCH")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["ok"]=false;
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    //导出用户修改的username
                    string username=any_cast<string>(inf.ctx["username"]);
                    //访问数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :改名失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd(con,
                        "UPDATE t_user "
                        "SET username = :username "
                        "WHERE id = :id"
                    );
                    
                    cmd.Param("username").setAsString() =username.c_str();
                    cmd.Param("id").setAsLong()=userinf[userinf_fd].id;

                    cmd.Execute();

                    //long rows = cmd.RowsAffected();

                    //if (rows != 1)
                    //{
                    //    lf->writeLog("username="+userinf[userinf_fd].username+" :换头像失败，没有行影响 id="+to_string(userinf[userinf_fd].id));
                    //    backPool(con);
                    //    string res;
                    //    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","换头像失败，服务器错误"),res);
                    //    if(k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400")))
                    //        return 1;
                    //    else
                    //        return -2;
                    //}
                    
                    lf->writeLog("username="+userinf[userinf_fd].username+" :改名成功 新名字:"+username);
                    userinf[userinf_fd].username=username;
                    backPool(con);
                    //发回json
                    
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"display_name",username),res);
                    inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        inf.ctx["ok"]=true;
                        inf.ctx["username"]=move(username);
                        inf.ctx["userinf_fd"]=move(userinf_fd);
                        return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :改名失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        // ===== 2️⃣ username 重复（唯一索引）=====
                        else if (native == 1062)
                        {
                   
                            lf->writeLog("username="+userinf[userinf_fd].username+" :改名失败失败，用户名已存在 username="+username);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","用户名已存在"),res);
                            
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;

                            backPool(con);

                            return 1;
                         }
                        // ===== 4️⃣ 其他 SQL 错误 =====
                        else
                        {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :改名失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/user/profile",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "PATCH")
        {
            if(any_cast<bool>(inf.ctx["ok"]))
            {
                //修改内存
                changeUsername(userinf[any_cast<long>(inf.ctx["userinf_fd"])].username,any_cast<string>(inf.ctx["username"]));
                userinf[any_cast<long>(inf.ctx["userinf_fd"])].username=any_cast<string>(inf.ctx["username"]);
            }
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/user/password_params",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    inf.ctx["token"]=move(token);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else
            return -2;
    });
    httpserver->setFunction("/api/user/password_params",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :查询失败，token长度不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :查询失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/user/password_params",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    //访问数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :查询失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd(con,
                        "SELECT password_kdf,password_iterations,password_salt "
                        "FROM t_user "
                        "WHERE id = :id"
                    );
                    
                    cmd.Param("id").setAsLong()=userinf[userinf_fd].id;
                    cmd.Execute();
                    if(!cmd.FetchNext())
                    {
                        lf->writeLog("查询失败 username="+userinf[userinf_fd].username);
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    string password_kdf        = cmd.Field("password_kdf").asString().GetMultiByteChars();
                    long password_iterations_num = cmd.Field("password_iterations").asLong();
                    string password_salt       = cmd.Field("password_salt").asString().GetMultiByteChars();
                    
                    lf->writeLog("username="+userinf[userinf_fd].username+" :查询成功");
                    
                    backPool(con);
                    //发回json
                    
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"passwordHash",JsonHelper::toJsonArray(JsonHelper::createJson("kdf",password_kdf,"iterations",password_iterations_num,"salt",password_salt))),res);
                    inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :查询失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","查询失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :查询失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","查询失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/user/password_params",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    
    httpserver->setFunction("/api/messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    inf.ctx["token"]=move(token);
                    //解析peer
                    string peer="";
                    HttpStringUtil::get_value_str(inf.para,peer,"peer");
                    inf.ctx["peer"]=move(peer);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else
            return -2;
    });
    httpserver->setFunction("/api/messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :查询信息历史失败，token长度不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :信息历史失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;

            if(any_cast<string>(inf.ctx["peer"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :信息历史失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    //得到id
                    long peer_id=getUserId(any_cast<string>(inf.ctx["peer"]));
                    if(peer_id==-1)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :查询历史消息失败，找不到该用户");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","查询历史消息失败，找不到该用户"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;//肮脏
                    }
                    //访问数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :查询历史消息失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd1(con,
                        "SELECT created_at,payload_json "
                        "FROM t_message "
                        "WHERE from_id = :1 AND to_id=:2"
                    );
                    
                    cmd1.Param("1").setAsLong()=userinf[userinf_fd].id;
                    cmd1.Param("2").setAsLong()=peer_id;
                    cmd1.Execute();

                    string json1="[ ]";
                    while(cmd1.FetchNext())
                    {
                        //string a=cmd1.Field("payload_json").asString().GetMultiByteChars();
                        string b=decodePayloadJsonStored(cmd1.Field("payload_json").asString().GetMultiByteChars());
                        if(b.empty())
                        {
                            lf->writeLog("private_messages: 跳过非法payload_json");
                            continue;
                        }
                        json1=JsonHelper::jsonAdd(json1,JsonHelper::createArray(JsonHelper::toJsonArray(b)));
                       //cout<<json1<<endl;
                        //json1=JsonHelper::jsonAdd(json1,JsonHelper::createArray(JsonHelper::toJsonArray(JsonHelper::createJson("from",userinf[userinf_fd].username,"ts",cmd1.Field("created_at").asLong(),"payload",JsonHelper::toJsonArray(hexDecode(cmd1.Field("payload_json").asString().GetMultiByteChars()))))));
                    }
                    
                    SACommand cmd2(con,
                        "SELECT created_at,payload_json "
                        "FROM t_message "
                        "WHERE from_id = :1 AND to_id=:2"
                    );
                    
                    cmd2.Param("2").setAsLong()=userinf[userinf_fd].id;
                    cmd2.Param("1").setAsLong()=peer_id;
                    cmd2.Execute();
                    //cout<<"**********"<<endl;
                    string json2="[ ]";
                    while(cmd2.FetchNext())
                    {
                        //cout<<"????"<<endl;
                        //string a=cmd2.Field("payload_json").asString().GetMultiByteChars();
                        string b=decodePayloadJsonStored(cmd2.Field("payload_json").asString().GetMultiByteChars());
                        if(b.empty())
                        {
                            lf->writeLog("private_messages: 跳过非法payload_json");
                            continue;
                        }
                        json2=JsonHelper::jsonAdd(json2,JsonHelper::createArray(JsonHelper::toJsonArray(b)));
                        //cout<<json2<<endl;
                        //json2=JsonHelper::jsonAdd(json2,JsonHelper::createArray(JsonHelper::toJsonArray(JsonHelper::createJson("from",any_cast<string>(inf.ctx["peer"]),"ts",cmd2.Field("created_at").asLong(),"payload",JsonHelper::toJsonArray(hexDecode(cmd2.Field("payload_json").asString().GetMultiByteChars()))))));
                    }

                    
                    lf->writeLog("username="+userinf[userinf_fd].username+" :查询历史消息成功");
                    
                    backPool(con);
                    //发回json
                    string json;
                    if(json1!="[ ]"&&json2!="[ ]")
                        json=JsonHelper::jsonAdd(json1,json2);
                    else if(json1=="[ ]")
                        json=json2;
                    else if(json2=="[ ]")
                        json=json1;
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"message",JsonHelper::toJsonArray(json)),res);
                    //cout<<res<<endl;
                    inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :查询历史消息失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","查询历史消息失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :查询历史消息失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","查询历史消息失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });

    httpserver->setFunction("/api/groups",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
        }
        else if(inf.type=="GET" || inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    inf.ctx["token"]=move(token);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return -2;
    });
    httpserver->setFunction("/api/groups",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type=="GET" || inf.type=="POST")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6)
            {
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                return -2;
            }
            inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7);
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())
            {
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                return -2;
            }
            inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/groups",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type=="GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    long ufd=any_cast<long>(inf.ctx["userinf_fd"]);
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                        SACommand cmd(con,
                        "SELECT g.id,g.name,g.owner_id,COUNT(gm2.user_id) AS member_count "
                        "FROM t_group_member gm "
                        "JOIN t_group g ON gm.group_id=g.id "
                        "LEFT JOIN t_group_member gm2 ON gm2.group_id=g.id "
                        "WHERE gm.user_id=:uid "
                        "GROUP BY g.id,g.name,g.owner_id "
                        "ORDER BY g.id DESC");
                        cmd.Param("uid").setAsLong() = userinf[ufd].id;
                        cmd.Execute();
                        string arr="[ ]";
                        while(cmd.FetchNext())
                        {
                            long gid=cmd.Field("id").asLong();
                            string gname=cmd.Field("name").asString().GetMultiByteChars();
                            long owner=cmd.Field("owner_id").asLong();
                            long epoch=getGroupEpoch(gid);
                            long member_count=cmd.Field("member_count").asLong();
                            string owner_name="";
                            auto oi=idToInf.find(owner);
                            if(oi!=idToInf.end()) owner_name=oi->second.username;
                            arr=JsonHelper::jsonAdd(arr,JsonHelper::createArray(JsonHelper::toJsonArray(
                                JsonHelper::createJson("id",gid,"name",gname,"owner_username",owner_name,"member_count",member_count,"key_epoch",epoch)
                            )));
                        }
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"groups",JsonHelper::toJsonArray(arr)),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    catch(SAException &e)
                    {
                        lf->writeLog("groups 查询失败，SQL错误 native="+to_string(e.ErrNativeCode())+" err="+string(e.ErrText().GetMultiByteChars()));
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                },
                k,
                inf
            );
            return 0;
        }
        if(inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    long ufd=any_cast<long>(inf.ctx["userinf_fd"]);
                    string name,members_csv;
                    JsonHelper::getValue(inf.body,name,"value","name");
                    JsonHelper::getValue(inf.body,members_csv,"value","members_csv");
                    name=trimCopy(name);
                    if(name.empty())
                    {
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","群名称不能为空"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                        SACommand ins(con,
                        "INSERT INTO t_group(name,owner_id) VALUES(:n,:o)");
                        ins.Param("n").setAsString() = name.c_str();
                        ins.Param("o").setAsLong() = userinf[ufd].id;
                        ins.Execute();
                        while (ins.FetchNext()) {}

                        SACommand idCmd(con,"SELECT LAST_INSERT_ID()");
                        idCmd.Execute();
                        idCmd.FetchNext();
                        long gid=idCmd.Field(1).asLong();
                        while (idCmd.FetchNext()) {}

                        SACommand insOwner(con,
                        "INSERT IGNORE INTO t_group_member(group_id,user_id,role) VALUES(:g,:u,:r)");
                        insOwner.Param("g").setAsLong() = gid;
                        insOwner.Param("u").setAsLong() = userinf[ufd].id;
                        insOwner.Param("r").setAsString() = "owner";
                        insOwner.Execute();
                        while (insOwner.FetchNext()) {}

                        vector<string> members=splitCSV(members_csv);
                        unordered_set<long> pushIds;
                        pushIds.insert(userinf[ufd].id);
                        for(const auto &uname:members)
                        {
                            long uid=getUserId(uname);
                            if(uid==-1 || uid==userinf[ufd].id) continue;
                            SACommand insM(con,
                            "INSERT IGNORE INTO t_group_member(group_id,user_id,role) VALUES(:g,:u,:r)");
                            insM.Param("g").setAsLong() = gid;
                            insM.Param("u").setAsLong() = uid;
                            insM.Param("r").setAsString() = "member";
                            insM.Execute();
                            while (insM.FetchNext()) {}
                            pushIds.insert(uid);
                        }
                        backPool(con);

                        idToGroup.insert_or_assign(gid,GroupInfo{gid,name,userinf[ufd].id,1});
                        for(auto uid:pushIds) addGroupMemberMem(gid,uid);
                        for(auto uid:pushIds)
                        {
                            auto oi=idtoIndex.find(uid);
                            if(oi!=idtoIndex.end())
                            {
                                wsserver->sendMessage(userinf[oi->second].fd,"{ \"type\": \"groups_refresh\" }");
                            }
                        }

                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"group_id",gid,"key_epoch",1),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    catch(SAException &e)
                    {
                        long native = e.ErrNativeCode();
                        string err = e.ErrText().GetMultiByteChars();
                        lf->writeLog("username=" + userinf[ufd].username + " :创建群失败，SQL错误 native=" + to_string(native) + " err=" + err);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","创建群失败"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/groups",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type=="GET"||inf.type=="POST")
        {
            if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                return -2;
            return 1;
        }
        return 1;
    });

    httpserver->setFunction("/api/groups/invite",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            return -2;
        }
        if(inf.type!="POST") return -2;
        httpserver->putTask(
            [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                string token="";
                HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                if(token=="" || token.length()<=6)
                {
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                token=token.substr(7);
                auto ti=tokenToIndex.find(token);
                if(ti==tokenToIndex.end())
                {
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                string gidStr,username;
                JsonHelper::getValue(inf.body,gidStr,"value","group_id");
                JsonHelper::getValue(inf.body,username,"value","username");
                long gid=-1;
                NumberStringConvertUtil::toLong(gidStr,gid);
                long uid=getUserId(username);
                if(gid<=0 || uid<=0)
                {
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","参数错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                if(!isGroupMember(gid,userinf[ti->second].id))
                {
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","无权限"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                SAConnection *con=getConn();
                if(con==nullptr)
                {
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                try{
                    SACommand ins(con,"INSERT IGNORE INTO t_group_member(group_id,user_id,role) VALUES(:g,:u,:r)");
                    ins.Param("g").setAsLong()=gid;
                    ins.Param("u").setAsLong()=uid;
                    ins.Param("r").setAsString()="member";
                    ins.Execute();
                    backPool(con);
                    addGroupMemberMem(gid,uid);
                    string epErr;
                    if(!bumpGroupEpochDBAndMem(gid,epErr))
                    {
                        lf->writeLog("group invite: epoch bump failed gid="+to_string(gid)+" err="+epErr);
                    }
                    notifyGroupEpochChanged(gid);
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"key_epoch",getGroupEpoch(gid)),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }catch(...){
                    disconnectDB(con);
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","邀请失败"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
            },
            k,
            inf
        );
        return 0;
    });

    httpserver->setFunction("/api/groups/remove",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            return -2;
        }
        if(inf.type!="POST") return -2;
        httpserver->putTask(
            [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                string token="";
                HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                if(token=="" || token.length()<=6){ k.sendBack(JsonHelper::createJson("success",false,"error","token错误"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")); return 1; }
                token=token.substr(7);
                auto ti=tokenToIndex.find(token);
                if(ti==tokenToIndex.end()){ k.sendBack(JsonHelper::createJson("success",false,"error","token错误"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")); return 1; }
                string gidStr,username;
                JsonHelper::getValue(inf.body,gidStr,"value","group_id");
                JsonHelper::getValue(inf.body,username,"value","username");
                long gid=-1;
                NumberStringConvertUtil::toLong(gidStr,gid);
                long uid=getUserId(username);
                auto gi=idToGroup.find(gid);
                if(gid<=0 || uid<=0 || gi==idToGroup.end() || gi->second.owner_id!=userinf[ti->second].id || uid==gi->second.owner_id)
                {
                    k.sendBack(JsonHelper::createJson("success",false,"error","无权限"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                SAConnection *con=getConn();
                if(con==nullptr){ k.sendBack(JsonHelper::createJson("success",false,"error","服务器错误"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")); return 1; }
                try{
                    SACommand del(con,"DELETE FROM t_group_member WHERE group_id=:g AND user_id=:u");
                    del.Param("g").setAsLong()=gid;
                    del.Param("u").setAsLong()=uid;
                    del.Execute();
                    backPool(con);
                    unordered_set<long> extraNotify{uid};
                    groupMembers[gid].erase(uid);
                    userGroups[uid].erase(gid);
                    string epErr;
                    if(!bumpGroupEpochDBAndMem(gid,epErr))
                    {
                        lf->writeLog("group remove: epoch bump failed gid="+to_string(gid)+" err="+epErr);
                    }
                    notifyGroupEpochChanged(gid,extraNotify);
                    k.sendBack(JsonHelper::createJson("success",true,"key_epoch",getGroupEpoch(gid)),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }catch(...){
                    disconnectDB(con);
                    k.sendBack(JsonHelper::createJson("success",false,"error","移除失败"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
            },
            k,
            inf
        );
        return 0;
    });

    httpserver->setFunction("/api/groups/leave",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            return -2;
        }
        if(inf.type!="POST") return -2;
        httpserver->putTask(
            [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                string token="";
                HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                if(token=="" || token.length()<=6){ k.sendBack(JsonHelper::createJson("success",false,"error","token错误"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")); return 1; }
                token=token.substr(7);
                auto ti=tokenToIndex.find(token);
                if(ti==tokenToIndex.end()){ k.sendBack(JsonHelper::createJson("success",false,"error","token错误"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")); return 1; }
                string gidStr;
                JsonHelper::getValue(inf.body,gidStr,"value","group_id");
                long gid=-1;
                NumberStringConvertUtil::toLong(gidStr,gid);
                auto gi=idToGroup.find(gid);
                if(gid<=0 || gi==idToGroup.end() || !isGroupMember(gid,userinf[ti->second].id))
                {
                    k.sendBack(JsonHelper::createJson("success",false,"error","无权限"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                if(gi->second.owner_id==userinf[ti->second].id)
                {
                    k.sendBack(JsonHelper::createJson("success",false,"error","群主不能退群"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                SAConnection *con=getConn();
                if(con==nullptr){ k.sendBack(JsonHelper::createJson("success",false,"error","服务器错误"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")); return 1; }
                try{
                    SACommand del(con,"DELETE FROM t_group_member WHERE group_id=:g AND user_id=:u");
                    del.Param("g").setAsLong()=gid;
                    del.Param("u").setAsLong()=userinf[ti->second].id;
                    del.Execute();
                    backPool(con);
                    long leaveUid=userinf[ti->second].id;
                    unordered_set<long> extraNotify{leaveUid};
                    groupMembers[gid].erase(userinf[ti->second].id);
                    userGroups[userinf[ti->second].id].erase(gid);
                    string epErr;
                    if(!bumpGroupEpochDBAndMem(gid,epErr))
                    {
                        lf->writeLog("group leave: epoch bump failed gid="+to_string(gid)+" err="+epErr);
                    }
                    notifyGroupEpochChanged(gid,extraNotify);
                    k.sendBack(JsonHelper::createJson("success",true,"key_epoch",getGroupEpoch(gid)),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }catch(...){
                    disconnectDB(con);
                    k.sendBack(JsonHelper::createJson("success",false,"error","退群失败"),HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
            },
            k,
            inf
        );
        return 0;
    });

    httpserver->setFunction("/api/groups/members",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            return -2;
        }
        if(inf.type!="GET") return -2;
        httpserver->putTask(
            [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                string token="",gidStr;
                HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                HttpStringUtil::get_value_str(inf.para,gidStr,"group_id");
                if(token=="" || token.length()<=6)
                {
                    string res; JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                token=token.substr(7);
                auto ti=tokenToIndex.find(token);
                if(ti==tokenToIndex.end())
                {
                    string res; JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                long gid=-1;
                NumberStringConvertUtil::toLong(gidStr,gid);
                if(gid<=0 || !isGroupMember(gid,userinf[ti->second].id))
                {
                    string res; JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","无权限"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                SAConnection *con=getConn();
                if(con==nullptr)
                {
                    string res; JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                try
                {
                    SACommand cmd(con,
                    "SELECT u.username,u.avatar_url,u.pubkey_alg,u.pubkey_curve,u.pubkey_spki FROM t_group_member gm "
                    "JOIN t_user u ON gm.user_id=u.id "
                    "WHERE gm.group_id=:g ORDER BY gm.id ASC");
                    cmd.Param("g").setAsLong()=gid;
                    cmd.Execute();
                    string arr="[ ]";
                    while(cmd.FetchNext())
                    {
                        string uname=cmd.Field("username").asString().GetMultiByteChars();
                        string avatar=cmd.Field("avatar_url").asString().GetMultiByteChars();
                        string pkAlg=cmd.Field("pubkey_alg").asString().GetMultiByteChars();
                        string pkCurve=cmd.Field("pubkey_curve").asString().GetMultiByteChars();
                        string pkSpki=cmd.Field("pubkey_spki").asString().GetMultiByteChars();
                        arr=JsonHelper::jsonAdd(arr,JsonHelper::createArray(JsonHelper::toJsonArray(
                            JsonHelper::createJson(
                                "username",uname,
                                "display_name",uname,
                                "avatar_url",avatar,
                                "public_key",JsonHelper::toJsonArray(JsonHelper::createJson("alg",pkAlg,"curve",pkCurve,"spki",pkSpki))
                            )
                        )));
                    }
                    backPool(con);
                    string res; JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"members",JsonHelper::toJsonArray(arr)),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
                catch(...)
                {
                    disconnectDB(con);
                    string res; JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                    return 1;
                }
            },
            k,
            inf
        );
        return 0;
    });

    httpserver->setFunction("/api/group_messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
        }
        if(inf.type=="GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    string token="",gid;
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    HttpStringUtil::get_value_str(inf.para,gid,"group_id");
                    inf.ctx["token"]=move(token);
                    inf.ctx["gid"]=move(gid);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return -2;
    });
    httpserver->setFunction("/api/group_messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type=="GET")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6)
            {
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                return -2;
            }
            inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7);
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())
            {
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                return -2;
            }
            long gid=-1;
            NumberStringConvertUtil::toLong(any_cast<string>(inf.ctx["gid"]),gid);
            if(gid<=0 || !isGroupMember(gid,userinf[ii->second].id))
            {
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","无权限"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400"));
                return -2;
            }
            inf.ctx["userinf_fd"]=ii->second;
            inf.ctx["group_id_long"]=gid;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/group_messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type=="GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    string dbErr;
                    MYSQL *con=connectDBNative(dbErr);
                    if(con==nullptr)
                    {
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    long gid=any_cast<long>(inf.ctx["group_id_long"]);
                    string sql=
                        "SELECT payload_json FROM t_group_message "
                        "WHERE group_id=" + to_string(gid) + " AND JSON_VALID(payload_json)=1 "
                        "ORDER BY id ASC LIMIT 1000";
                    if(mysql_query(con,sql.c_str())!=0)
                    {
                        string err=mysql_error(con);
                        lf->writeLog("group_messages 查询失败，MySQL错误 err="+err);
                        mysql_close(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    MYSQL_RES *rs=mysql_store_result(con);
                    if(!rs)
                    {
                        string err=mysql_error(con);
                        lf->writeLog("group_messages 取结果失败，MySQL错误 err="+err);
                        mysql_close(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    string arr="[";
                    bool first=true;
                    MYSQL_ROW row;
                    while((row=mysql_fetch_row(rs))!=nullptr)
                    {
                        unsigned long *lens=mysql_fetch_lengths(rs);
                        if(!row[0] || !lens) continue;
                        string raw(row[0], lens[0]); // keep raw bytes, no charset conversion
                        string obj=trimCopy(raw);
                        if(obj.empty() || obj[0] != '{')
                        {
                            lf->writeLog("group_messages: 跳过非法payload_json");
                            continue;
                        }
                        if(!first) arr += ",";
                        arr += obj;
                        first=false;
                    }
                    mysql_free_result(rs);
                    mysql_close(con);
                    arr += "]";

                    string res="{\"success\":true,\"message\":"+arr+"}";
                    inf.ctx["body"]=move(res);
                    inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400");
                    inf.ctx["code"]=string("200");
                    inf.ctx["close"]=false;
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/group_messages",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type=="GET")
        {
            if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                return -2;
            return 1;
        }
        return 1;
    });
    
    httpserver->setFunction("/api/friends",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else if(inf.type=="DELETE")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    //解析json数据
                    string username;

                    JsonHelper::getValue(inf.body,username,"value","username");
                    
                    //存入
                    inf.ctx["username"]=move(username);

                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else
            return -2;
    });
    httpserver->setFunction("/api/friends",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :查询朋友列表失败，token长度不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :查询朋友列表失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        else if(inf.type=="DELETE")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :删除好友失败，token长度不合法或者没有请求体");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
                    
                    if(any_cast<string>(inf.ctx["username"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :删除好友失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :删除好友失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    //访问数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :查询朋友列表失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd(con,
                    R"(
                        SELECT
                        u.id,
                        u.username,
                        u.avatar_url,
                        u.pubkey_alg,
                        u.pubkey_curve,
                        u.pubkey_spki
                        FROM t_friend f
                        JOIN t_user u
                        ON u.id = IF(f.user_id = :uid, f.peer_id, f.user_id)
                        WHERE
                        (f.user_id = :uid OR f.peer_id = :uid)
                        AND f.status = 'accepted'
                    )");

                    cmd.Param("uid").setAsLong() = userinf[userinf_fd].id;

                    cmd.Execute();
                    string dataArray="[ ]";
                    while(cmd.FetchNext())
                    {
                        uint64_t peer_id = cmd.Field("id").asLong();
                        std::string username = cmd.Field("username").asString().GetMultiByteChars();
                        std::string avatar = cmd.Field("avatar_url").asString().GetMultiByteChars();
                        std::string alg = cmd.Field("pubkey_alg").asString().GetMultiByteChars();
                        std::string curve = cmd.Field("pubkey_curve").asString().GetMultiByteChars();
                        std::string spki = cmd.Field("pubkey_spki").asString().GetMultiByteChars();
                        //判断是否是unreaded
                        bool unreaded;
                        long latest;
                        long read_ts;
                        bool ok=false;
                        auto ii=pop_test.find(userinf[userinf_fd].id);
                        if(ii==pop_test.end())
                        {
                            unreaded=true;
                            ok=true;
                        }
                        else
                        {
                            auto jj=ii->second.find(peer_id);
                            if(jj==ii->second.end())
                            {
                                unreaded=false;
                                ok=true;
                            }
                            else
                            {
                                read_ts=jj->second;
                            }
                        }

                        auto aa=latest_test.find(peer_id);
                        if(aa==latest_test.end())
                        {
                            unreaded=false;
                            ok=true;
                        }
                        else
                        {
                            auto bb=aa->second.find(userinf[userinf_fd].id);
                            if(bb==aa->second.end())
                            {
                                unreaded=false;
                                ok=true;
                            }
                            else
                            {
                                latest=bb->second;
                            }
                        }

                        if(!ok)
                        {
                            if(latest<=read_ts)
                            {
                                unreaded=false;
                            }
                            else
                                unreaded=true;
                        }
                       // cout<<ok<<endl;
                      // cout<<latest<<endl;
                       // cout<<read_ts<<endl;
                        //接入json
                        string jsonSingle=JsonHelper::createArray(JsonHelper::toJsonArray(JsonHelper::createJson("username",username,"display_name",username,"avatar_url",avatar,"has_unread",unreaded,"public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",alg,"crv",curve,"spki",spki)))));
                        dataArray=JsonHelper::jsonAdd(dataArray,jsonSingle);
                    }
                    
                    SACommand cmdd(con,
R"(
SELECT
    u.id,
    u.username,
    u.avatar_url
FROM t_friend f
JOIN t_user u
ON u.id = IF(f.user_id = :uid, f.peer_id, f.user_id)
WHERE
    (f.user_id = :uid OR f.peer_id = :uid)
AND
(
   (f.status='pending_out' AND f.peer_id=:uid)
OR (f.status='pending_in'  AND f.user_id=:uid)
)
)");

                    cmdd.Param("uid").setAsLong() = userinf[userinf_fd].id;

                    cmdd.Execute();
                    string dataArrayR="[ ]";
                    while(cmdd.FetchNext())
                    {
                        uint64_t peer_id = cmdd.Field("id").asULong();
                        std::string username = cmdd.Field("username").asString().GetMultiByteChars();
                        std::string avatar = cmdd.Field("avatar_url").asString().GetMultiByteChars();

                        // push 到 pending list
                        string jsonSingle=JsonHelper::createArray(JsonHelper::toJsonArray(JsonHelper::createJson("username",username,"display_name",username,"avatar_url",avatar,"direction","in")));
                        dataArrayR=JsonHelper::jsonAdd(dataArrayR,jsonSingle);
                    }
                   
                    lf->writeLog("username="+userinf[userinf_fd].username+" :查询好友列表成功");
                    
                    backPool(con);
                    //发回json
                    
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"friends",JsonHelper::toJsonArray(dataArray),"requests",JsonHelper::toJsonArray(dataArrayR)),res);
                    inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :查询好友列表失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","查询好友列表失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :查询好友列表失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","查询好友列表失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else if(inf.type=="DELETE")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["idA"]=-1;
                    inf.ctx["idB"]=-1;
                    inf.ctx["ws"]=false;
                    inf.ctx["ok"]=false;
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    if(userinf[userinf_fd].username==any_cast<string>(inf.ctx["username"]))
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :删除好友失败，不能自己删自己");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","删除好友失败，自己删自己"),res);//肮脏
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    //数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :删除好友失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","删除好友失败，没能获取到数据库连接池的连接"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {

                        //检查用户是否存在 获取用户id
                        SACommand findUser(con,
                        "SELECT id FROM t_user WHERE username=:name");
                        string peer_username=any_cast<string>(inf.ctx["username"]);
                        findUser.Param("name").setAsString() = peer_username.c_str();
                        findUser.Execute();

                    if(!findUser.FetchNext())
                    {
                        // 用户不存在
                        lf->writeLog("username="+userinf[userinf_fd].username+" :删除好友失败,用户不存在");
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","删除好友失败,用户不存在"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    long peer_id = findUser.Field(1).asLong();
                    

                    while (findUser.FetchNext()) {}

                    
                    //检查关系是否存在
                    SACommand check(con,
                    "SELECT status FROM t_friend WHERE user_id=:a AND peer_id=:b AND status=:c");
                    if(peer_id<userinf[userinf_fd].id)
                    {
                        check.Param("a").setAsLong() = peer_id;
                        check.Param("b").setAsLong() = userinf[userinf_fd].id;
                        check.Param("c").setAsString() = "accepted";
                    }
                    else
                    {
                        check.Param("b").setAsLong() = peer_id;
                        check.Param("a").setAsLong() = userinf[userinf_fd].id;
                        check.Param("c").setAsString() = "accepted";
                    }
                    check.Execute();

                    if(!check.FetchNext())//不存在 失败
                    {
                        
                        lf->writeLog("username="+userinf[userinf_fd].username+" :好友关系不存在 请注意不要做一些肮脏的事情");
                        backPool(con);
                        //发回json
                    
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","好友关系不存在，请注意不要做一些肮脏的事情"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    else//存在 删了
                    {
                        
                                while (check.FetchNext()) {}
                            // 数据库
                            SACommand upd(con,
                             "UPDATE t_friend SET status='none' WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :删除好友成功");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            inf.ctx["idA"]=peer_id;
                            inf.ctx["idB"]=userinf[userinf_fd].id;
                            inf.ctx["ws"]=true;
                            inf.ctx["ok"]=true;
                            auto index=idtoIndex.find(peer_id);
                            if(index==idtoIndex.end())
                                inf.ctx["peer_fd"]=(long)-1;
                            else
                                inf.ctx["peer_fd"]=(long)index->second;
                            //inf.ctx["peer_msg"]=JsonHelper::createJson("type","friend_removed","username",peer_username);
                            return 1;
                            
                        
                        
                        
             
                    }
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :删除好友失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","删除好友失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :删除好友失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","删除好友失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "GET" || inf.type=="DELETE")
        {
            if(inf.type=="DELETE")
            {
            //修改内存
                if(any_cast<bool>(inf.ctx["ok"]))
                    deleteRelationship(any_cast<long>(inf.ctx["idA"]),any_cast<long>(inf.ctx["idB"]));
                
                //发websocket消息
                if(any_cast<bool>(inf.ctx["ws"]))
                    //wsserver->sendMessage(any_cast<long>(inf.ctx["peer_fd"]),any_cast<string>(inf.ctx["peer_msg"]));
                    wsserver->sendMessage(any_cast<long>(inf.ctx["peer_fd"]),"{ \"type\": \"friends_refresh\" }");
            }
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/user",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
            {
                
                return 1;
            }
            else
            {
                
                return -2;
            }
        }
        else if(inf.type=="DELETE")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    //解析json
                    string password_kdf="";
                    string password_iterations="";
                    string password_salt="";
                    string password_hash="";
                    string password="";

                    long password_iterations_num;


                    JsonHelper::getValue(inf.body,password,"value","passwordHash");
                    JsonHelper::getValue(password,password_kdf,"value","kdf");
                    JsonHelper::getValue(password,password_iterations,"value","iterations");
                    JsonHelper::getValue(password,password_salt,"value","salt");
                    JsonHelper::getValue(password,password_hash,"value","hash");

                    NumberStringConvertUtil::toLong(password_iterations,password_iterations_num);
                    
                    inf.ctx["password_kdf"]=password_kdf;
                    inf.ctx["password_iterations_num"]=password_iterations_num;
                    inf.ctx["password_salt"]=password_salt;
                    inf.ctx["password_hash"]=password_hash;
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else
            return -2;
    });
    httpserver->setFunction("/api/user",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "DELETE")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :注销失败，token长度不合法或没带上请求体");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
                    
                    if(any_cast<long>(inf.ctx["password_iterations_num"])==-1||any_cast<string>(inf.ctx["password_kdf"])==""||any_cast<string>(inf.ctx["password_salt"])==""||any_cast<string>(inf.ctx["password_hash"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" ：注销失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :注销失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/user",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "DELETE")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["ok"]=false;
                    inf.ctx["username"]=" ";
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    
                    //访问数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :注销失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","注销失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                        //检查密码是否正确
                    SACommand cmd(con,
                        "SELECT username "
                        "FROM t_user "
                        "WHERE id = :id AND password_kdf=:password_kdf AND password_iterations=:password_iterations AND password_salt=:password_salt AND password_hash=:password_hash"
                    );
                    
                    cmd.Param("id").setAsLong() = userinf[userinf_fd].id;
                    cmd.Param("password_kdf").setAsString() = (any_cast<string>(inf.ctx["password_kdf"])).c_str();
                    cmd.Param("password_iterations").setAsLong() = any_cast<long>(inf.ctx["password_iterations_num"]);
                    cmd.Param("password_salt").setAsString() = (any_cast<string>(inf.ctx["password_salt"])).c_str();
                    cmd.Param("password_hash").setAsString() = (any_cast<string>(inf.ctx["password_hash"])).c_str();
                    
                    cmd.Execute();
                    if(!cmd.FetchNext())
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :注销失败,密码错误");
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","注销失败，密码错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    while (cmd.FetchNext()) {}

                    
                    lf->writeLog("username="+userinf[userinf_fd].username+" :密码验证成功");
                    
                    //删除用户
                    SACommand cmdd(con,
                        "DELETE FROM t_user "
                        "WHERE id = :id"
                    );
                    
                    cmdd.Param("id").setAsLong() = userinf[userinf_fd].id;
                    
                    cmdd.Execute();

                    long rows = cmdd.RowsAffected();

                    if(rows!=1)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :注销失败");
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","注销失败，数据库错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    //踢人下线
                    lf->writeLog("username="+userinf[userinf_fd].username+" :注销成功");


                    backPool(con);
                    //发回json
                    
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true),res);
                    inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        inf.ctx["username"]=userinf[userinf_fd].username;
                        inf.ctx["ok"]=true;
                        return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :注销失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改名失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :注销失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","注销失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","DELETE, PATCH, GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/user",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "DELETE")
        {
            //修改内存
            if(any_cast<bool>(inf.ctx["ok"]))
                deleteUser(any_cast<string>(inf.ctx["username"]));
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/user/password",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    //解析json数据

                    string password_old="";
                    string password_new="";
                    string privatekey="";

                    string password_kdf_old="";
                    string password_iterations_old="";
                    string password_salt_old="";
                    string password_hash_old="";

                    string password_kdf_new="";
                    string password_iterations_new="";
                    string password_salt_new="";
                    string password_hash_new="";

                    string prikey_kdf="";
                    string prikey_iterations="";
                    string prikey_salt="";
                    string prikey_cipher="";
                    string prikey_iv="";
                    string prikey_ciphertext="";

                    long password_iterations_num_old;
                    long password_iterations_num_new;
                    long prikey_iterations_num;

                    JsonHelper::getValue(inf.body,password_old,"value","oldPasswordHash");
                    JsonHelper::getValue(password_old,password_kdf_old,"value","kdf");
                    JsonHelper::getValue(password_old,password_iterations_old,"value","iterations");
                    JsonHelper::getValue(password_old,password_salt_old,"value","salt");
                    JsonHelper::getValue(password_old,password_hash_old,"value","hash");

                    JsonHelper::getValue(inf.body,password_new,"value","newPasswordHash");
                    JsonHelper::getValue(password_new,password_kdf_new,"value","kdf");
                    JsonHelper::getValue(password_new,password_iterations_new,"value","iterations");
                    JsonHelper::getValue(password_new,password_salt_new,"value","salt");
                    JsonHelper::getValue(password_new,password_hash_new,"value","hash");

                    JsonHelper::getValue(inf.body,privatekey,"value","newEncryptedPrivateKey");
                    JsonHelper::getValue(privatekey,prikey_kdf,"value","kdf");
                    JsonHelper::getValue(privatekey,prikey_iterations,"value","iterations");
                    JsonHelper::getValue(privatekey,prikey_salt,"value","salt");
                    JsonHelper::getValue(privatekey,prikey_cipher,"value","cipher");
                    JsonHelper::getValue(privatekey,prikey_iv,"value","iv");
                    JsonHelper::getValue(privatekey,prikey_ciphertext,"value","ciphertext");

                    NumberStringConvertUtil::toLong(password_iterations_old,password_iterations_num_old);
                    NumberStringConvertUtil::toLong(password_iterations_new,password_iterations_num_new);
                    NumberStringConvertUtil::toLong(prikey_iterations,prikey_iterations_num);
                    
                    //存入
                    inf.ctx["password_kdf_old"]=move(password_kdf_old);
                    inf.ctx["password_iterations_num_old"]=move(password_iterations_num_old);
                    inf.ctx["password_salt_old"]=move(password_salt_old);
                    inf.ctx["password_hash_old"]=move(password_hash_old);

                    inf.ctx["password_kdf_new"]=move(password_kdf_new);
                    inf.ctx["password_iterations_num_new"]=move(password_iterations_num_new);
                    inf.ctx["password_salt_new"]=move(password_salt_new);
                    inf.ctx["password_hash_new"]=move(password_hash_new);

                    inf.ctx["prikey_kdf"]=move(prikey_kdf);
                    inf.ctx["prikey_iterations_num"]=move(prikey_iterations_num);
                    inf.ctx["prikey_salt"]=move(prikey_salt);
                    inf.ctx["prikey_cipher"]=move(prikey_cipher);
                    inf.ctx["prikey_iv"]=move(prikey_iv);
                    inf.ctx["prikey_ciphertext"]=move(prikey_ciphertext);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else 
            return -2;
    });
    httpserver->setFunction("/api/user/password",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :改密码失败，token长度不合法或者没有请求体");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
                    
                    if(any_cast<long>(inf.ctx["password_iterations_num_old"])==-1||
                    any_cast<long>(inf.ctx["password_iterations_num_new"])==-1||
                    any_cast<long>(inf.ctx["prikey_iterations_num"])==-1||
                    any_cast<string>(inf.ctx["password_kdf_old"])==""||
                    any_cast<string>(inf.ctx["password_salt_old"])==""||
                    any_cast<string>(inf.ctx["password_hash_old"])==""||
                    any_cast<string>(inf.ctx["password_kdf_new"])==""||
                    any_cast<string>(inf.ctx["password_salt_new"])==""||
                    any_cast<string>(inf.ctx["password_hash_new"])==""||
                    any_cast<string>(inf.ctx["prikey_cipher"])==""||
                    any_cast<string>(inf.ctx["prikey_ciphertext"])==""||
                    any_cast<string>(inf.ctx["prikey_iv"])==""||
                    any_cast<string>(inf.ctx["prikey_kdf"])==""||
                    any_cast<string>(inf.ctx["prikey_salt"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :改密码失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :改密码失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/user/password",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    
                    //数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :改密码失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改密码失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {

                        //检查原密码是否正确
                        SACommand cmd(con,
                        "SELECT username "
                        "FROM t_user "
                        "WHERE id = :id AND password_kdf=:password_kdf AND password_iterations=:password_iterations AND password_salt=:password_salt AND password_hash=:password_hash"
                    );
                    
                    cmd.Param("id").setAsLong() = userinf[userinf_fd].id;
                    cmd.Param("password_kdf").setAsString() = (any_cast<string>(inf.ctx["password_kdf_old"])).c_str();
                    cmd.Param("password_iterations").setAsLong() = any_cast<long>(inf.ctx["password_iterations_num_old"]);
                    cmd.Param("password_salt").setAsString() = (any_cast<string>(inf.ctx["password_salt_old"])).c_str();
                    cmd.Param("password_hash").setAsString() = (any_cast<string>(inf.ctx["password_hash_old"])).c_str();
                    
                    cmd.Execute();
                    if(!cmd.FetchNext())
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :改密码失败,密码错误");
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改密码失败，密码错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    while (cmd.FetchNext()) {}

                    
                    lf->writeLog("username="+userinf[userinf_fd].username+" :密码验证成功");
                    //存入新数据
                    SACommand cmdd(con,
                        "UPDATE t_user "
                        "SET password_kdf = :password_kdf, password_iterations= :password_iterations_num_new, password_salt= :password_salt_new, password_hash= :password_hash_new, prikey_kdf= :prikey_kdf, prikey_iterations=:prikey_iterations_num, prikey_salt=:prikey_salt, prikey_cipher=:prikey_cipher, prikey_iv=:prikey_iv, prikey_ciphertext=:prikey_ciphertext "
                        "WHERE id = :id"
                    );
                    
                    cmdd.Param("password_kdf").setAsString() =(any_cast<string>(inf.ctx["password_kdf_new"])).c_str();
                    cmdd.Param("password_iterations_num_new").setAsLong() =(any_cast<long>(inf.ctx["password_iterations_num_new"]));
                    cmdd.Param("password_salt_new").setAsString() =(any_cast<string>(inf.ctx["password_salt_new"])).c_str();
                    cmdd.Param("password_hash_new").setAsString() =(any_cast<string>(inf.ctx["password_hash_new"])).c_str();
                    cmdd.Param("prikey_kdf").setAsString() =(any_cast<string>(inf.ctx["prikey_kdf"])).c_str();
                    cmdd.Param("prikey_iterations_num").setAsLong() =(any_cast<long>(inf.ctx["prikey_iterations_num"]));
                    cmdd.Param("prikey_salt").setAsString() =(any_cast<string>(inf.ctx["prikey_salt"])).c_str();
                    cmdd.Param("prikey_cipher").setAsString() =(any_cast<string>(inf.ctx["prikey_cipher"])).c_str();
                    cmdd.Param("prikey_iv").setAsString() =(any_cast<string>(inf.ctx["prikey_iv"])).c_str();
                    cmdd.Param("prikey_ciphertext").setAsString() =(any_cast<string>(inf.ctx["prikey_ciphertext"])).c_str();
                    cmdd.Param("id").setAsLong()=userinf[userinf_fd].id;
                    cmdd.Execute();
                    
                    lf->writeLog("username="+userinf[userinf_fd].username+" :改密码成功");
                    backPool(con);
                    //发回json
                    
                    string res;
                    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true),res);
                    inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :改密码失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改密码失败失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :改密码失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","改密码失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/user/password",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/request",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    //解析json数据
                    string username;

                    JsonHelper::getValue(inf.body,username,"value","username");
                    
                    //存入
                    inf.ctx["username"]=move(username);

                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else 
            return -2;
    });
    httpserver->setFunction("/api/friends/request",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {   
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :发送好友请求失败，token长度不合法或者没有请求体");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
                    
                    if(any_cast<string>(inf.ctx["username"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :发送好友请求失败失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :发送好友请求失败失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/request",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["ok"]=false;
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    if(userinf[userinf_fd].username==any_cast<string>(inf.ctx["username"]))
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，自己加自己");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","发送好友请求失败，自己加自己"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    //数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","发送好友请求失败，没能获取到数据库连接池的连接"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {

                        //检查用户是否存在 获取用户id
                        SACommand findUser(con,
                        "SELECT id,pubkey_alg,pubkey_curve,pubkey_spki FROM t_user WHERE username=:name");
                        string peer_username=any_cast<string>(inf.ctx["username"]);
                        findUser.Param("name").setAsString() = peer_username.c_str();
                        findUser.Execute();

                    if(!findUser.FetchNext())
                    {
                        // 用户不存在
                        lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败,用户不存在");
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","发送好友请求失败,用户不存在"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    long peer_id = findUser.Field(1).asLong();
                    string pubkey_alg=findUser.Field("pubkey_alg").asString().GetMultiByteChars();
                    string pubkey_curve=findUser.Field("pubkey_curve").asString().GetMultiByteChars();
                    string pubkey_spki=findUser.Field("pubkey_spki").asString().GetMultiByteChars();
                    while (findUser.FetchNext()) {}

                    //申请信息存入好友表
                    //检查是否存在
                    SACommand check(con,
                    "SELECT status FROM t_friend WHERE user_id=:a AND peer_id=:b");
                    if(peer_id<userinf[userinf_fd].id)
                    {
                        check.Param("a").setAsLong() = peer_id;
                        check.Param("b").setAsLong() = userinf[userinf_fd].id;
                    }
                    else
                    {
                        check.Param("b").setAsLong() = peer_id;
                        check.Param("a").setAsLong() = userinf[userinf_fd].id;
                    }
                    check.Execute();

                    if(!check.FetchNext())//不存在 插入
                    {
                        while (check.FetchNext()) {}
                        string status;
                        
                        SACommand ins(con,
                        "INSERT INTO t_friend(user_id,peer_id,status) VALUES(:a,:b,:s)");

                        if(peer_id<userinf[userinf_fd].id)
                        {
                            ins.Param("a").setAsLong() = peer_id;
                            ins.Param("b").setAsLong() = userinf[userinf_fd].id;
                            status="pending_in";
                            ins.Param("s").setAsString() = status.c_str();
                        }
                        else
                        {
                            ins.Param("b").setAsLong() = peer_id;
                            ins.Param("a").setAsLong() = userinf[userinf_fd].id;
                            status="pending_out";
                            ins.Param("s").setAsString() = status.c_str();
                        }

                        ins.Execute();
                        lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求成功");
                    
                        backPool(con);
                        //发回json
                    
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"status","pending"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        inf.ctx["ok"]=true;
                            auto index=idtoIndex.find(peer_id);
                            if(index==idtoIndex.end())
                            {
                                inf.ctx["peer_fd"]=(long)-1; 
                            }
                            else
                            {
                                inf.ctx["peer_fd"]=(long)index->second; 
                            }
                        return 1;
                    }
                    else//存在 分情况
                    {
                        string old = check.Field("status").asString().GetMultiByteChars();
                        if(old == "accepted")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，已经是朋友了");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","已经是朋友了"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        if(old == "none")
                        {
                            string status;
                            while (check.FetchNext()) {}
                            // 对方之前加你 —— 直接接受
                            SACommand upd(con,
                             "UPDATE t_friend SET status=:s WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                                status="pending_in";
                                upd.Param("s").setAsString() = status.c_str();
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                                status="pending_out";
                                upd.Param("s").setAsString() = status.c_str();
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求成功");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"status","pending"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            inf.ctx["ok"]=true;
                            auto index=idtoIndex.find(peer_id);
                            if(index==idtoIndex.end())
                            {
                                inf.ctx["peer_fd"]=(long)-1; 
                            }
                            else
                            {
                                inf.ctx["peer_fd"]=(long)index->second; 
                            }
                            return 1;
                        }
                        if(old == "blocked")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，黑名单状态");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","黑名单状态"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        if(old == "pending_out")
                        {
                            if(userinf[userinf_fd].id<peer_id)
                            {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，已经发送过了");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","已经发送过了"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                            else// 对方之前加你 直接接受
                            {
                                while (check.FetchNext()) {}
                            // 对方之前加你 —— 直接接受
                            SACommand upd(con,
                             "UPDATE t_friend SET status='accepted' WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求成功，对方早就添加你了 直接成为好友");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"status","accepted","public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",pubkey_alg,"crv",pubkey_curve,"spki",pubkey_spki))),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            inf.ctx["ok"]=true;
                            auto index=idtoIndex.find(peer_id);
                            if(index==idtoIndex.end())
                            {
                                inf.ctx["peer_fd"]=(long)-1; 
                            }
                            else
                            {
                                inf.ctx["peer_fd"]=(long)index->second; 
                            }
                            //inf.ctx["peer_msg"]=JsonHelper::createJson("type","friend_accepted","username",userinf[userinf_fd].username,"display_name",userinf[userinf_fd].username,"avatar_url",userinf[userinf_fd].avatar_url,"public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",userinf[userinf_fd].kty,"crv",userinf[userinf_fd].crv,"spki",userinf[userinf_fd].spki)));
                            return 1;
                            }
                        }
                        if(old == "pending_in")
                        {
                            if(userinf[userinf_fd].id<peer_id)
                            {
                            while (check.FetchNext()) {}
                            // 对方之前加你 —— 直接接受
                            SACommand upd(con,
                             "UPDATE t_friend SET status='accepted' WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求成功，对方早就添加你了 直接成为好友");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"status","accepted","public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",pubkey_alg,"crv",pubkey_curve,"spki",pubkey_spki))),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            inf.ctx["ok"]=true;
                            auto index=idtoIndex.find(peer_id);
                            if(index==idtoIndex.end())
                            {
                                inf.ctx["peer_fd"]=(long)-1; 
                            }
                            else
                            {
                                inf.ctx["peer_fd"]=(long)index->second; 
                            }
                            //inf.ctx["peer_msg"]=JsonHelper::createJson("type","friend_accepted","username",userinf[userinf_fd].username,"display_name",userinf[userinf_fd].username,"avatar_url",userinf[userinf_fd].avatar_url,"public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",userinf[userinf_fd].kty,"crv",userinf[userinf_fd].crv,"spki",userinf[userinf_fd].spki)));
                            return 1;
                            }
                            else
                            {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，已经发送过了");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","已经发送过了"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                        }
             
                    }
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","发送好友请求失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :发送好友请求失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","发送好友请求失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/request",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<bool>(inf.ctx["ok"]))
            {
               // wsserver->sendMessage(any_cast<long>(inf.ctx["peer_fd"]),any_cast<string>(inf.ctx["peer_msg"]));
               wsserver->sendMessage(any_cast<long>(inf.ctx["peer_fd"]),"{ \"type\": \"friends_refresh\" }");
            }
            
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/accept",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    //解析json数据
                    string username;

                    JsonHelper::getValue(inf.body,username,"value","username");
                    
                    //存入
                    inf.ctx["username"]=move(username);

                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else 
            return -2;
    });
    httpserver->setFunction("/api/friends/accept",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {   
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :同意好友请求失败，token长度不合法或者没有请求体");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
                    
                    if(any_cast<string>(inf.ctx["username"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :同意好友请求失败失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :同意好友请求失败失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/accept",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["idA"]=-1;
                    inf.ctx["idB"]=-1;
                    inf.ctx["ws"]=false;
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    if(userinf[userinf_fd].username==any_cast<string>(inf.ctx["username"]))
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败，不能自己加自己");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败，自己加自己"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    //数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败，没能获取到数据库连接池的连接"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {

                        //检查用户是否存在 获取用户id
                        SACommand findUser(con,
                        "SELECT id,pubkey_alg,pubkey_curve,pubkey_spki FROM t_user WHERE username=:name");
                        string peer_username=any_cast<string>(inf.ctx["username"]);
                        findUser.Param("name").setAsString() = peer_username.c_str();
                        findUser.Execute();

                    if(!findUser.FetchNext())
                    {
                        // 用户不存在
                        lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败,用户不存在");
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败,用户不存在"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    long peer_id = findUser.Field(1).asLong();
                    string pubkey_alg=findUser.Field("pubkey_alg").asString().GetMultiByteChars();
                    string pubkey_curve=findUser.Field("pubkey_curve").asString().GetMultiByteChars();
                    string pubkey_spki=findUser.Field("pubkey_spki").asString().GetMultiByteChars();

                    while (findUser.FetchNext()) {}


                    //通过信息存入好友表
                    //检查申请是否存在
                    SACommand check(con,
                    "SELECT status FROM t_friend WHERE user_id=:a AND peer_id=:b");
                    if(peer_id<userinf[userinf_fd].id)
                    {
                        check.Param("a").setAsLong() = peer_id;
                        check.Param("b").setAsLong() = userinf[userinf_fd].id;
                    }
                    else
                    {
                        check.Param("b").setAsLong() = peer_id;
                        check.Param("a").setAsLong() = userinf[userinf_fd].id;
                    }
                    check.Execute();

                    if(!check.FetchNext())//不存在 失败
                    {
                        
                    
                        backPool(con);
                        //发回json
                    
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败,对方根本没加你1，请注意不要做一些肮脏的事情"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    else//存在 分情况
                    {
                        string old = check.Field("status").asString().GetMultiByteChars();
                        if(old == "accepted")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败，已经是朋友了 请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败，已经是朋友了 请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        if(old == "none")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败,对方根本没加你21，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败,对方根本没加你，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                        }
                        if(old == "blocked")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败，黑名单状态，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败，黑名单状态，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        if(old == "pending_out")
                        {
                            if(userinf[userinf_fd].id<peer_id)
                            {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败,对方根本没加你21，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败,对方根本没加你，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                            else
                            {
                                while (check.FetchNext()) {}
                            // 对方之前加你 —— 直接接受
                            SACommand upd(con,
                             "UPDATE t_friend SET status='accepted' WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求成功");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"status","accepted","public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",pubkey_alg,"crv",pubkey_curve,"spki",pubkey_spki))),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            inf.ctx["idA"]=peer_id;
                            inf.ctx["idB"]=userinf[userinf_fd].id;
                            inf.ctx["ws"]=true;
                            auto index=idtoIndex.find(peer_id);
                            if(index==idtoIndex.end())
                            {
                                inf.ctx["peer_fd"]=(long)-1; 
                            }
                            else
                            {
                                inf.ctx["peer_fd"]=(long)index->second; 
                            }
                            //inf.ctx["peer_msg"]=JsonHelper::createJson("type","friend_accepted","username",userinf[userinf_fd].username,"display_name",userinf[userinf_fd].username,"avatar_url",userinf[userinf_fd].avatar_url,"public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",userinf[userinf_fd].kty,"crv",userinf[userinf_fd].crv,"spki",userinf[userinf_fd].spki)));
                            return 1;
                            }
                        }
                        if(old == "pending_in")
                        {
                            if(userinf[userinf_fd].id<peer_id)
                            {
                            while (check.FetchNext()) {}
                            // 对方之前加你 —— 直接接受
                            SACommand upd(con,
                             "UPDATE t_friend SET status='accepted' WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求成功");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true,"status","accepted","public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",pubkey_alg,"crv",pubkey_curve,"spki",pubkey_spki))),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            inf.ctx["idA"]=peer_id;
                            inf.ctx["idB"]=userinf[userinf_fd].id;
                            inf.ctx["ws"]=true;
                            auto index=idtoIndex.find(peer_id);
                            if(index==idtoIndex.end())
                            {
                                inf.ctx["peer_fd"]=(long)-1; 
                            }
                            else
                            {
                                inf.ctx["peer_fd"]=(long)index->second; 
                            }
                            //inf.ctx["peer_msg"]=JsonHelper::createJson("type","friend_accepted","username",userinf[userinf_fd].username,"display_name",userinf[userinf_fd].username,"avatar_url",userinf[userinf_fd].avatar_url,"public_key",JsonHelper::toJsonArray(JsonHelper::createJson("kty",userinf[userinf_fd].kty,"crv",userinf[userinf_fd].crv,"spki",userinf[userinf_fd].spki)));
                            return 1;
                            }
                            else
                            {
                                lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败,对方根本没加你22，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败,对方根本没加你，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                        }
             
                    }
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :通过好友请求失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","通过好友请求失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/accept",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            //ws转发
            if(any_cast<bool>(inf.ctx["ws"]))
            {
                //wsserver->sendMessage(any_cast<long>(inf.ctx["peer_fd"]),any_cast<string>(inf.ctx["peer_msg"]));
                wsserver->sendMessage(any_cast<long>(inf.ctx["peer_fd"]),"{ \"type\": \"friends_refresh\" }");
            }
            //更新内存
            addRelationship(any_cast<long>(inf.ctx["idA"]),any_cast<long>(inf.ctx["idB"]));
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/reject",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-type, x-filename","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type=="POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //解析token
                    string token="";
                    HttpStringUtil::get_value_header(inf.header,token,"Authorization");
                    
                    inf.ctx["token"]=move(token);
                    //解析json数据
                    string username;

                    JsonHelper::getValue(inf.body,username,"value","username");
                    
                    //存入
                    inf.ctx["username"]=move(username);

                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        else 
            return -2;
    });
    httpserver->setFunction("/api/friends/reject",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {   
            if(any_cast<string>(inf.ctx["token"])==""||any_cast<string>(inf.ctx["token"]).length()<=6||inf.body.length()==0)
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :拒绝好友请求失败，token长度不合法或者没有请求体");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
                    inf.ctx["token"]=any_cast<string>(inf.ctx["token"]).substr(7); 
                    
                    if(any_cast<string>(inf.ctx["username"])=="")
                    {
                        lf->writeLog("fd="+to_string(inf.fd)+" :拒绝好友请求失败失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                        return -2;
                    }
            //检验token
            auto ii=tokenToIndex.find(any_cast<string>(inf.ctx["token"]));
            if(ii==tokenToIndex.end())//找不到token
            {
                lf->writeLog("fd="+to_string(inf.fd)+" :拒绝好友请求失败失败，token错误");
                string res;
                JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","token错误，请勿做一些肮脏的事情。"),res);
                k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400"));
                //严重错误 必须关掉连接 说明他绕过了前端
                return -2;
            }
            else
                inf.ctx["userinf_fd"]=ii->second;
            return 1;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/reject",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    //导出在userinf中的下标
                    long userinf_fd=any_cast<long>(inf.ctx["userinf_fd"]);
                    if(userinf[userinf_fd].username==any_cast<string>(inf.ctx["username"]))
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败，不能自己拒绝自己");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败，自己拒绝自己"),res);//肮脏
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    //数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败，没能获取到数据库连接池的连接"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {

                        //检查用户是否存在 获取用户id
                        SACommand findUser(con,
                        "SELECT id FROM t_user WHERE username=:name");
                        string peer_username=any_cast<string>(inf.ctx["username"]);
                        findUser.Param("name").setAsString() = peer_username.c_str();
                        findUser.Execute();

                    if(!findUser.FetchNext())
                    {
                        // 用户不存在
                        lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败,用户不存在");
                        backPool(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败,用户不存在"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }

                    long peer_id = findUser.Field(1).asLong();
                    

                    while (findUser.FetchNext()) {}

                    //通过信息存入好友表
                    //检查申请是否存在
                    SACommand check(con,
                    "SELECT status FROM t_friend WHERE user_id=:a AND peer_id=:b");
                    if(peer_id<userinf[userinf_fd].id)
                    {
                        check.Param("a").setAsLong() = peer_id;
                        check.Param("b").setAsLong() = userinf[userinf_fd].id;
                    }
                    else
                    {
                        check.Param("b").setAsLong() = peer_id;
                        check.Param("a").setAsLong() = userinf[userinf_fd].id;
                    }
                    check.Execute();

                    if(!check.FetchNext())//不存在 失败
                    {
                        
                    
                        backPool(con);
                        //发回json
                    
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败,对方根本没加你1，请注意不要做一些肮脏的事情"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    else//存在 分情况
                    {
                        string old = check.Field("status").asString().GetMultiByteChars();
                        if(old == "accepted")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败，已经是朋友了 请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败，已经是朋友了 请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        if(old == "none")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败,对方根本没加你21，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败,对方根本没加你，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                        }
                        if(old == "blocked")
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败，黑名单状态，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败，黑名单状态，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }
                        if(old == "pending_out")
                        {
                            if(userinf[userinf_fd].id<peer_id)
                            {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败,对方根本没加你21，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败,对方根本没加你，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                            else
                            {
                                while (check.FetchNext()) {}
                            // 对方之前加你 
                            SACommand upd(con,
                             "UPDATE t_friend SET status='none' WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求成功");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                        }
                        if(old == "pending_in")
                        {
                            if(userinf[userinf_fd].id<peer_id)
                            {
                            while (check.FetchNext()) {}
                            // 对方之前加你
                            SACommand upd(con,
                             "UPDATE t_friend SET status='none' WHERE user_id=:a AND peer_id=:b");

                            if(peer_id<userinf[userinf_fd].id)
                            {
                                upd.Param("a").setAsLong() = peer_id;
                                upd.Param("b").setAsLong() = userinf[userinf_fd].id;
                            }
                            else
                            {
                                upd.Param("b").setAsLong() = peer_id;
                                upd.Param("a").setAsLong() = userinf[userinf_fd].id;
                            }

                            upd.Execute();
                            lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求成功");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",true),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                            else
                            {
                                lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败,对方根本没加你22，请注意不要做一些肮脏的事情");
                    
                            backPool(con);
                            //发回json
                    
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败,对方根本没加你，请注意不要做一些肮脏的事情"),res);
                            inf.ctx["body"]=move(res);
                            inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                            inf.ctx["code"]=string("200");
                            inf.ctx["close"]=false;
                            return 1;
                            }
                        }
             
                    }
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败，服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("username="+userinf[userinf_fd].username+" :拒绝好友请求失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","拒绝好友请求失败，服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","authorization, content-typ, x-filename","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
        return 1;
    });
    httpserver->setFunction("/api/friends/reject",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->setFunction("/register",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if (inf.type == "OPTIONS")
        {
            if(k.sendBack("",HttpStringUtil::createHeader("Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400")))
                return 1;
            else
                return -2;
            
        }
        else if(inf.type == "POST")
        {
            httpserver->putTask(
                [](HttpServerFDHandler& k, HttpRequestInformation& inf) -> int {
                    inf.ctx["ok"]=false;
                    
                    //解析json数据
                    string username="";
                    string password="";
                    string publickey="";
                    string privatekey="";

                    string password_kdf="";
                    string password_iterations="";
                    string password_salt="";
                    string password_hash="";

                    string pubkey_alg="";
                    string pubkey_curve="";
                    string pubkey_spki="";

                    string prikey_kdf="";
                    string prikey_iterations="";
                    string prikey_salt="";
                    string prikey_cipher="";
                    string prikey_iv="";
                    string prikey_ciphertext="";

                    long password_iterations_num;
                    long prikey_iterations_num;

                    JsonHelper::getValue(inf.body,username,"value","username");

                    JsonHelper::getValue(inf.body,password,"value","passwordHash");
                    JsonHelper::getValue(password,password_kdf,"value","kdf");
                    JsonHelper::getValue(password,password_iterations,"value","iterations");
                    JsonHelper::getValue(password,password_salt,"value","salt");
                    JsonHelper::getValue(password,password_hash,"value","hash");

                    JsonHelper::getValue(inf.body,privatekey,"value","encryptedPrivateKey");
                    JsonHelper::getValue(privatekey,prikey_kdf,"value","kdf");
                    JsonHelper::getValue(privatekey,prikey_iterations,"value","iterations");
                    JsonHelper::getValue(privatekey,prikey_salt,"value","salt");
                    JsonHelper::getValue(privatekey,prikey_cipher,"value","cipher");
                    JsonHelper::getValue(privatekey,prikey_iv,"value","iv");
                    JsonHelper::getValue(privatekey,prikey_ciphertext,"value","ciphertext");


                    JsonHelper::getValue(inf.body,publickey,"value","publicKey");
                    JsonHelper::getValue(publickey,pubkey_alg,"value","alg");
                    JsonHelper::getValue(publickey,pubkey_curve,"value","curve");
                    JsonHelper::getValue(publickey,pubkey_spki,"value","spki");

                    NumberStringConvertUtil::toLong(password_iterations,password_iterations_num);
                    NumberStringConvertUtil::toLong(prikey_iterations,prikey_iterations_num);
                    if(username==""||password_iterations_num==-1||prikey_iterations_num==-1||password_kdf==""||password_salt==""||password_hash==""||pubkey_alg==""||pubkey_curve==""||pubkey_spki==""||prikey_cipher==""||prikey_ciphertext==""||prikey_iv==""||prikey_kdf==""||prikey_salt=="")
                    {
                        lf->writeLog("注册失败，数据不合法");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","数据不合法，请勿做一些肮脏的事情。"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=true;
                        return 1;
                        //严重错误 必须关掉连接 说明他绕过了前端
                
                    }

                    //if (userId.size() > 64)
                    //{
                    //    lf->writeLog("注册失败，username 过长");
                    //    string res;
                    //    JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","用户名过长"),res);
                    //    k.sendBack(res,HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin","https://stephentaam.github.io","Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400"));
                        //严重错误 必须关掉连接 说明他绕过了前端
                    //    return -2;
                    //}

                    //插入到数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("注册失败，没能获取到数据库连接池的连接");
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd(con,
                        "INSERT INTO t_user ("
                        "username, "
                        "password_kdf, password_iterations, password_salt, password_hash, "
                        "pubkey_alg, pubkey_curve, pubkey_spki, "
                        "prikey_kdf, prikey_iterations, prikey_salt, prikey_cipher, prikey_iv, prikey_ciphertext"
                        ") VALUES ("
                        ":username, "
                        ":password_kdf, :password_iterations, :password_salt, :password_hash, "
                        ":pubkey_alg, :pubkey_curve, :pubkey_spki, "
                        ":prikey_kdf, :prikey_iterations, :prikey_salt, :prikey_cipher, :prikey_iv, :prikey_ciphertext"
                        ")"
                    );
                    cmd.Param("username").setAsString() = username.c_str();

                    /* password hash */
                    cmd.Param("password_kdf").setAsString()        = password_kdf.c_str();
                    cmd.Param("password_iterations").setAsLong()   = password_iterations_num;
                    cmd.Param("password_salt").setAsString()       = password_salt.c_str();
                    cmd.Param("password_hash").setAsString()       = password_hash.c_str();

                    /* public key */
                    cmd.Param("pubkey_alg").setAsString()   = pubkey_alg.c_str();
                    cmd.Param("pubkey_curve").setAsString() = pubkey_curve.c_str();
                    cmd.Param("pubkey_spki").setAsString()  = pubkey_spki.c_str();

                    /* encrypted private key */
                    cmd.Param("prikey_kdf").setAsString()        = prikey_kdf.c_str();
                    cmd.Param("prikey_iterations").setAsLong()   = prikey_iterations_num;
                    cmd.Param("prikey_salt").setAsString()       = prikey_salt.c_str();
                    cmd.Param("prikey_cipher").setAsString()     = prikey_cipher.c_str();
                    cmd.Param("prikey_iv").setAsString()         = prikey_iv.c_str();
                    cmd.Param("prikey_ciphertext").setAsString() = prikey_ciphertext.c_str();

                    cmd.Execute();
                    while (cmd.FetchNext()) {}

                    lf->writeLog("注册成功，username="+username);
                    //if(k.sendBack("{\"success\":true}",HttpStringUtil::createHeader("Connection","keep-alive","Content-Type","application/json; charset=utf-8")))
                    inf.ctx["body"]=JsonHelper::createJson("success",true);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        
                        SACommand idCmd(con,"SELECT LAST_INSERT_ID()");
                        idCmd.Execute();
                        idCmd.FetchNext();
                        while (idCmd.FetchNext()) {}
                        inf.ctx["ok"]=true;
                        inf.ctx["username"]=username;
                        long di=idCmd.Field(1).asLong();
                        inf.ctx["id"] = move(di);
                        inf.ctx["kty"]=move(pubkey_alg);
                        inf.ctx["crv"]=move(pubkey_curve);
                        inf.ctx["spki"]=move(pubkey_spki);
                    backPool(con);
                    return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("注册失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误，请再次尝试"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        return 1;
                        }

                        // ===== 2️⃣ username 重复（唯一索引）=====
                        if (native == 1062)
                        {
                            lf->writeLog("注册失败，用户名已存在 username="+username);
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","用户名已存在"),res);
                            
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                            backPool(con);
                            return 1;
                         }

                        // ===== 3️⃣ 字段长度不匹配（兜底）=====
                        if (native == 1406) // Data too long for column
                        {
                            lf->writeLog("注册失败，username 过长");
                            string res;
                            JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","用户名过长"),res);
                            inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=true;
                            backPool(con);
                            //严重错误 必须关掉连接 说明他绕过了前端
                            return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("注册失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("success",false,"error","服务器错误"),res);
                        inf.ctx["body"]=move(res);
                        inf.ctx["header"]=HttpStringUtil::createHeader("Content-Type","application/json; charset=utf-8","Access-Control-Allow-Origin",g_httpCfg.cors_allow_origin.c_str(),"Access-Control-Allow-Methods","GET, POST, OPTIONS","Access-Control-Allow-Headers","content-type","Access-Control-Max-Age","86400");
                        inf.ctx["code"]=string("200");
                        inf.ctx["close"]=false;
                        //backPool(con);
                        disconnectDB(con);
                        return 1;
                    }
                    
                },
                k,
                inf
            );
            return 0;
        }
        else
            return -2;
    });
    httpserver->setFunction("/register",[](HttpServerFDHandler &k,HttpRequestInformation &inf)->int
    {
        if(inf.type == "POST")
        {
            //修改内存
            if(any_cast<bool>(inf.ctx["ok"]))
                addUser(any_cast<string>(inf.ctx["username"]),any_cast<long>(inf.ctx["id"]),"/resources/avatars/default.png",any_cast<string>(inf.ctx["kty"]),any_cast<string>(inf.ctx["crv"]),any_cast<string>(inf.ctx["spki"]));
            if(any_cast<bool>(inf.ctx["close"])==false)
            {
                if(!k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"])))
                    return -2;
                return 1;
            }
            else
            {
                k.sendBack(any_cast<string>(inf.ctx["body"]),any_cast<string>(inf.ctx["header"]),any_cast<string>(inf.ctx["code"]));
                return -2;
            }
        }
        return 1;
    });
    httpserver->startListen(g_httpCfg.listen_port, g_httpCfg.listen_threads);


    //初始化ws登陆哈希表
    for(int ii=0;ii<1000000;ii++)
    {
        userinf[ii].id=-1;
        userinf[ii].token="";
        userinf[ii].username="";
    }

    wsserver = new WebSocketServer(
        g_wsCfg.maxFD,
        g_wsCfg.buffer_size_kb,
        g_wsCfg.finishQueue_cap,
        g_wsCfg.security_open,
        g_wsCfg.connectionNumLimit,
        g_wsCfg.connectionSecs,
        g_wsCfg.connectionTimes,
        g_wsCfg.requestSecs,
        g_wsCfg.requestTimes,
        g_wsCfg.checkFrequency,
        g_wsCfg.connectionTimeout
    );
    wsserver->setTLS(
        g_wsCfg.tls_cert.c_str(),
        g_wsCfg.tls_key.c_str(),
        g_wsCfg.tls_pwd.c_str(),
        g_wsCfg.tls_ca.c_str()
    );

    //设置连上来后就覆盖为未登陆
    wsserver->setStartFunction([](WebSocketServerFDHandler &k,WebSocketFDInformation &inf)->bool
    {
        userinf[inf.fd].fd=-1;
        userinf[inf.fd].id=-1;
        userinf[inf.fd].token="";
        userinf[inf.fd].username="";
        return true;
    });

    /*
     * WebSocket：全局兜底处理函数
     */
    wsserver->setGlobalSolveFunction(
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> bool {
            return k.sendMessage(inf.message); // echo
            //return k.sendMessage(JsonHelper::createJson("cmd","login_ok","uid","stephen"));
        }
    );
    /*
     * WebSocket：提取 key
     */
    wsserver->setGetKeyFunction(
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {

            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    string type="";
                    JsonHelper::getValue(inf.message,type,"value","type");
                    if(type=="")
                        return -2;
                    inf.ctx["key"]=move(type);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );
    wsserver->setFunction(
        "ping",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            k.sendMessage("pong");
            return 1;
        }
    );

    wsserver->setFunction(
         "login_start",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    //解析json
                    string username="";
                    JsonHelper::getValue(inf.message,username,"value","username");
                    if(username=="")
                        return -2;//肮脏
                    //查数据库
                    string password_kdf;
                    long password_iterations_num;
                    string password_salt;
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("login_start失败，没能获取到数据库连接池的连接");
                        
                        inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","服务器错误");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd(con,
                        "SELECT password_kdf, password_iterations, password_salt "
                        "FROM t_user "
                        "WHERE username = :username"
                    );
                    
                    cmd.Param("username").setAsString() = username.c_str();
                    cmd.Execute();
                    if(!cmd.FetchNext())
                    {
                        lf->writeLog("login_start失败，用户不存在 username="+username);
                        backPool(con);
                        
                        inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","用户不存在");
                        inf.ctx["close"]=false;
                        return 1;
                    }
                    password_kdf        = cmd.Field("password_kdf").asString().GetMultiByteChars();
                    password_iterations_num = cmd.Field("password_iterations").asLong();
                    password_salt       = cmd.Field("password_salt").asString().GetMultiByteChars();
                    lf->writeLog("login_start成功，username="+username);
                    backPool(con);
                    //发回json
                    
                    inf.ctx["message"]=JsonHelper::createJson("type","login_challenge","passwordHash",JsonHelper::toJsonArray(JsonHelper::createJson("kdf",password_kdf,"iterations",static_cast<int64_t>(password_iterations_num),"salt",password_salt)));
                    inf.ctx["close"]=false;
                    return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("login_start失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            
                            inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","服务器错误，请再次尝试");
                            inf.ctx["close"]=false;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("login_start失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","服务器错误");
                        inf.ctx["close"]=false;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );
    wsserver->setFunction(
        "login_start",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {

            if(!k.sendMessage(any_cast<string>(inf.ctx["message"])))
            {
                return -2;
            }
            if(any_cast<bool>(inf.ctx["close"]))
                return -2;
            else
                return 1;
        }
    );

    wsserver->setFunction(
         "login",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    //解析json
                    string username;
                    string password;

                    string password_kdf;
                    string password_iterations;
                    string password_salt;
                    string password_hash;

                    long password_iterations_num;

                    JsonHelper::getValue(inf.message,username,"value","username");

                    JsonHelper::getValue(inf.message,password,"value","passwordHash");
                    JsonHelper::getValue(password,password_kdf,"value","kdf");
                    JsonHelper::getValue(password,password_iterations,"value","iterations");
                    JsonHelper::getValue(password,password_salt,"value","salt");
                    JsonHelper::getValue(password,password_hash,"value","hash");

                    NumberStringConvertUtil::toLong(password_iterations,password_iterations_num);

                    if(username==""||password_iterations_num==-1||password_kdf==""||password_salt==""||password_hash=="")
                        return -2;
                    //查数据库
                    SAConnection *con=getConn();
                    if(con==nullptr)
                    {
                        lf->writeLog("login失败，没能获取到数据库连接池的连接");
                        inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","服务器错误");
                        inf.ctx["close"]=false;
                        inf.ctx["isLog"]=false;
                        return 1;
                    }
                    try
                    {
                    SACommand cmd(con,
                        "SELECT id,avatar_url,pubkey_alg,pubkey_curve,pubkey_spki,prikey_kdf,prikey_iterations,prikey_salt,prikey_cipher,prikey_iv,prikey_ciphertext "
                        "FROM t_user "
                        "WHERE username = :username AND password_kdf=:password_kdf AND password_iterations=:password_iterations AND password_salt=:password_salt AND password_hash=:password_hash"
                    );
                    
                    cmd.Param("username").setAsString() = username.c_str();
                    cmd.Param("password_kdf").setAsString() = password_kdf.c_str();
                    cmd.Param("password_iterations").setAsLong() = password_iterations_num;
                    cmd.Param("password_salt").setAsString() = password_salt.c_str();
                    cmd.Param("password_hash").setAsString() = password_hash.c_str();
                    
                    cmd.Execute();
                    if(!cmd.FetchNext())
                    {
                        lf->writeLog("login失败 username="+username);
                        backPool(con);
        
                        inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","用户信息有误");
                        inf.ctx["close"]=false;
                        inf.ctx["isLog"]=false;
                        return 1;
                    }
                    long id=cmd.Field("id").asLong();
                    string avatar_url=cmd.Field("avatar_url").asString().GetMultiByteChars();
                    string pubkey_alg = cmd.Field("pubkey_alg").asString().GetMultiByteChars();
                    string pubkey_curve = cmd.Field("pubkey_curve").asString().GetMultiByteChars();
                    string pubkey_spki = cmd.Field("pubkey_spki").asString().GetMultiByteChars();
                    string prikey_kdf = cmd.Field("prikey_kdf").asString().GetMultiByteChars();
                    long prikey_iterations_num = cmd.Field("prikey_iterations").asLong();
                    string prikey_salt = cmd.Field("prikey_salt").asString().GetMultiByteChars();
                    string prikey_cipher = cmd.Field("prikey_cipher").asString().GetMultiByteChars();
                    string prikey_iv = cmd.Field("prikey_iv").asString().GetMultiByteChars();
                    string prikey_ciphertext = cmd.Field("prikey_ciphertext").asString().GetMultiByteChars();

                    
                    backPool(con);
                    //发回json
                        //生成随机httptoken
                    string http_token;
                    RandomUtil::getRandomStr_base64(http_token,128);
                    lf->writeLog("login成功，username="+username+"  ,token="+http_token+" id="+to_string(id));
                    inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",true,"avatar_url",avatar_url,"http_token",http_token,"publicKey",JsonHelper::toJsonArray(JsonHelper::createJson("alg",pubkey_alg,"curve",pubkey_curve,"spki",pubkey_spki)),"encryptedPrivateKey",JsonHelper::toJsonArray(JsonHelper::createJson("kdf",prikey_kdf,"iterations",prikey_iterations_num,"salt",prikey_salt,"cipher",prikey_cipher,"iv",prikey_iv,"ciphertext",prikey_ciphertext)));
                    inf.ctx["close"]=false;
                    inf.ctx["isLog"]=true;
                    inf.ctx["id"]=id;
                    inf.ctx["token"]=move(http_token);
                    inf.ctx["username"]=move(username);
                    inf.ctx["avatar_url"]=move(avatar_url);
                    inf.ctx["pubkey_alg"]=move(pubkey_alg);
                    inf.ctx["pubkey_curve"]=move(pubkey_curve);
                    inf.ctx["pubkey_spki"]=move(pubkey_spki);
                    return 1;
                    }
                    catch (SAException &e)
                    {
                        long native = e.ErrNativeCode();

                        // ===== 1️⃣ 数据库连接 / 端口 / 断链问题 =====
                        if (native == 2006 || native == 2013 || native == 2055)
                        {
                            lf->writeLog("login失败，数据库连接异常 .关闭这个连接下次重连。");
                            disconnectDB(con);
                            inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","服务器错误，请再次尝试");
                            inf.ctx["close"]=false;
                            inf.ctx["isLog"]=false;
                            return 1;
                        }

                        // ===== 4️⃣ 其他 SQL 错误 =====
                        lf->writeLog("login失败，SQL 错误  msg="+string(e.ErrText().GetMultiByteChars()));
                        //backPool(con);
                        disconnectDB(con);
                        inf.ctx["message"]=JsonHelper::createJson("type","login_result","success",false,"error","服务器错误");
                        inf.ctx["close"]=false;
                        inf.ctx["isLog"]=false;
                        return 1;
                    }
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );
    
    wsserver->setFunction(
        "login",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            if(any_cast<bool>(inf.ctx["isLog"])==true)
            {
                userinf[inf.fd].fd=inf.fd;
                userinf[inf.fd].id=move(any_cast<long>(inf.ctx["id"]));
                userinf[inf.fd].token=move(any_cast<string>(inf.ctx["token"]));
                userinf[inf.fd].username=move(any_cast<string>(inf.ctx["username"]));
                userinf[inf.fd].avatar_url=move(any_cast<string>(inf.ctx["avatar_url"]));
                userinf[inf.fd].kty=move(any_cast<string>(inf.ctx["pubkey_alg"]));
                userinf[inf.fd].crv=move(any_cast<string>(inf.ctx["pubkey_curve"]));
                userinf[inf.fd].spki=move(any_cast<string>(inf.ctx["pubkey_spki"]));
                //填入token->fd哈希表
                tokenToIndex[userinf[inf.fd].token]=inf.fd;
                //踢掉其他连接
                auto ii=idtoIndex.find(userinf[inf.fd].id);
                if(ii!=idtoIndex.end())
                {
                    wsserver->close(ii->second);
                }
                idtoIndex[userinf[inf.fd].id]=inf.fd;
            }
            if(!k.sendMessage(any_cast<string>(inf.ctx["message"])))
            {
                return -2;
            }
            if(any_cast<bool>(inf.ctx["close"]))
                return -2;
            else
                return 1;
            return 1;
        }
    );

    
    wsserver->setFunction(
         "chat_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            
            //检查有没有登陆
            if(userinf[inf.fd].id==-1)
            {
                lf->writeLog("username="+userinf[inf.fd].username+"chat_send失败，没有登陆");//肮脏
                k.sendMessage(JsonHelper::createJson("type","chat_send","success",false,"error","没有登陆"));
                return -2;
            }
            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    //解析json
                    string to,ts,payload,kind,iv,ciphertext,mime,name,size,file_url;
                    JsonHelper::getValue(inf.message,to,"value","to");
                    JsonHelper::getValue(inf.message,ts,"value","ts");
                    JsonHelper::getValue(inf.message,payload,"value","payload");
                    JsonHelper::getValue(payload,kind,"value","kind");
                    JsonHelper::getValue(payload,iv,"value","iv");
                    JsonHelper::getValue(payload,ciphertext,"value","ciphertext");
                    JsonHelper::getValue(payload,file_url,"value","file_url");
                    JsonHelper::getValue(payload,mime,"value","mime");
                    JsonHelper::getValue(payload,name,"value","name");
                    JsonHelper::getValue(payload,size,"value","size");
                    long ssize;
                    long tss;
                    NumberStringConvertUtil::toLong(size,ssize);
                    NumberStringConvertUtil::toLong(ts,tss);
                    if(kind==""||((tss==-1||ssize==-1||to==""||iv==""||ciphertext==""||mime=="")&&(kind=="text"))||((tss==-1||ssize==-1||to==""||iv==""||mime==""||file_url=="")&&(kind=="file"||kind=="image'")))
                    {
                        lf->writeLog("username="+userinf[inf.fd].username+"chat_send失败，数据格式不对");//肮脏
                       string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("type","chat_send","success",false,"error","数据格式不对"),res);//肮脏
                        inf.ctx["msg"]=move(res);
                        inf.ctx["close"]=true;
                        return 1;
                    }
                    //判定用户和关系是否存在
                    long peer_id=getUserId(to);
                    if(peer_id==-1)
                    {
                        lf->writeLog("username="+userinf[inf.fd].username+"chat_send失败，用户不存在");//肮脏
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("type","chat_send","success",false,"error","用户不存在"),res);//肮脏
                        inf.ctx["msg"]=move(res);
                        inf.ctx["close"]=true;
                        return 1;
                    }
                    if(getRelationship(userinf[inf.fd].id,peer_id)!=1)
                    {
                        lf->writeLog("username="+userinf[inf.fd].username+"chat_send失败，好友关系不存在");//肮脏
                        string res;
                        JsonHelper::jsonToUTF8(JsonHelper::createJson("type","chat_send","success",false,"error","好友关系不存在"),res);//肮脏
                        inf.ctx["msg"]=move(res);
                        inf.ctx["close"]=true;
                        return 1;
                    }
                    //转化为fd
                    long peer_fd;
                    auto index=idtoIndex.find(peer_id);
                    if(index!=idtoIndex.end())
                    {
                        peer_fd=userinf[index->second].fd;
                    }
                    else
                    {
                        peer_fd=(long)-1;
                    }
                    //组织需要转发的内容 以及需要存入数据库的内容
                    inf.ctx["close"]=false;
                    inf.ctx["peer_msg"]=JsonHelper::createJson("from",userinf[inf.fd].username,"type","chat_recv","ts",tss,"payload",JsonHelper::toJsonArray(payload));
                    inf.ctx["peer_fd"]=move(peer_fd);
                    //inf.ctx["kind"]=move(kind);
                    //inf.ctx["iv"]=move(iv);
                    //inf.ctx["ciphertext"]=move(ciphertext);
                    //inf.ctx["mime"]=move(mime);
                   //inf.ctx["name"]=move(name);
                    //inf.ctx["size"]=move(ssize);
                    inf.ctx["ts"]=move(tss);
                    inf.ctx["tsstr"]=move(ts);
                    inf.ctx["close"]=false;
                    inf.ctx["to"]=move(to);
                    inf.ctx["from_id"]=userinf[inf.fd].id;
                    inf.ctx["to_id"]=move(peer_id);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );
    
    wsserver->setFunction(
        "chat_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            if(any_cast<bool>(inf.ctx["close"])==true)
            {
                k.sendMessage(any_cast<string>(inf.ctx["msg"]));
                return -2;
            }
            else
            {
                //先尝试直接转发
                wsserver->sendMessage(any_cast<long>(inf.ctx["peer_fd"]),any_cast<string>(inf.ctx["peer_msg"]));               
            }
            return 1;
        }
    );

    wsserver->setFunction(
        "chat_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    inf.ctx["ok"]=false;
                    string dbErr;
                    MYSQL *con=connectDBNative(dbErr);
                    if(con==nullptr)
                    {
                        lf->writeLog("发送消息失败，数据库连接失败: " + dbErr);
                        inf.ctx["ok"]=false;
                        return 1;
                    }

                    auto escape = [&](const string &src)->string {
                        string out;
                        out.resize(src.size()*2 + 1);
                        unsigned long n=mysql_real_escape_string(con,&out[0],src.c_str(),(unsigned long)src.size());
                        out.resize(n);
                        return out;
                    };

                    long fromId=any_cast<long>(inf.ctx["from_id"]);
                    long toId=any_cast<long>(inf.ctx["to_id"]);
                    string payload=any_cast<string>(inf.ctx["peer_msg"]);
                    string payloadEsc=escape(payload);
                    string tsstr=any_cast<string>(inf.ctx["tsstr"]);

                    string sql=
                        "INSERT INTO t_message(from_id,to_id,payload_json,ts) VALUES("
                        + to_string(fromId) + ","
                        + to_string(toId) + ","
                        "'" + payloadEsc + "',"
                        + tsstr + ")";

                    if(mysql_query(con,sql.c_str())!=0)
                    {
                        string err=mysql_error(con);
                        lf->writeLog("发送消息失败，MySQL错误 msg="+err);
                        mysql_close(con);
                        inf.ctx["ok"]=false;
                        return 1;
                    }

                    inf.ctx["ok"]=true;
                    inf.ctx["idd"]=(long)mysql_insert_id(con);
                    lf->writeLog("存入数据库成功");
                    mysql_close(con);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );

    wsserver->setFunction(
        "chat_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            
            //发回消息 & 存入latest表
            
            if(any_cast<bool>(inf.ctx["ok"]))
            {
                k.sendMessage(JsonHelper::createJson("type","chat_send","success",true,"client_ts",any_cast<long>(inf.ctx["ts"]),"server_ts",any_cast<long>(inf.ctx["ts"]),"message_id",any_cast<long>(inf.ctx["idd"]),"to",any_cast<string>(inf.ctx["to"])));
                //cout<<"latest更新 from :"<<any_cast<long>(inf.ctx["from_id"])<<" to:"<<any_cast<long>(inf.ctx["to_id"])<<" ts:"<<any_cast<long>(inf.ctx["ts"])<<endl;
                        auto ii=latest_test.find(any_cast<long>(inf.ctx["from_id"]));
                        if(ii==latest_test.end())
                        {
                            latest_test.emplace(any_cast<long>(inf.ctx["from_id"]),unordered_map<long,long>{{any_cast<long>(inf.ctx["to_id"]),any_cast<long>(inf.ctx["ts"])}});
                        }
                        else
                        {
                            ii->second.insert_or_assign(any_cast<long>(inf.ctx["to_id"]),any_cast<long>(inf.ctx["ts"]));
                        }
            }
            else
                k.sendMessage(JsonHelper::createJson("type","chat_send","success",false,"client_ts",any_cast<long>(inf.ctx["ts"]),"server_ts",any_cast<long>(inf.ctx["ts"]),"message_id",-1,"to",any_cast<string>(inf.ctx["to"])));
            return 1;
        }
    );

    wsserver->setFunction(
        "group_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            if(userinf[inf.fd].id==-1)
            {
                k.sendMessage(JsonHelper::createJson("type","group_send","success",false,"error","没有登陆"));
                return -2;
            }
            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    string group_id,ts,payload,key_epoch;
                    JsonHelper::getValue(inf.message,group_id,"value","group_id");
                    JsonHelper::getValue(inf.message,ts,"value","ts");
                    JsonHelper::getValue(inf.message,key_epoch,"value","key_epoch");
                    if(!extractJsonObjectField(inf.message,"payload",payload))
                    {
                        inf.ctx["close"]=true;
                        inf.ctx["msg"]=JsonHelper::createJson("type","group_send","success",false,"error","payload格式错误");
                        return 1;
                    }
                    long gid=-1,tss=-1,epoch=-1;
                    NumberStringConvertUtil::toLong(group_id,gid);
                    NumberStringConvertUtil::toLong(ts,tss);
                    NumberStringConvertUtil::toLong(key_epoch,epoch);
                    string payloadTrim=trimCopy(payload);
                    if(gid<=0 || tss<=0 || payloadTrim.empty() || epoch<=0)
                    {
                        inf.ctx["close"]=true;
                        inf.ctx["msg"]=JsonHelper::createJson("type","group_send","success",false,"error","数据格式不对","need_key_epoch",getGroupEpoch(gid));
                        return 1;
                    }
                    if(payloadTrim.front()!='{' || payloadTrim.back()!='}')
                    {
                        inf.ctx["close"]=true;
                        inf.ctx["msg"]=JsonHelper::createJson("type","group_send","success",false,"error","payload格式错误");
                        return 1;
                    }
                    if(!isGroupMember(gid,userinf[inf.fd].id))
                    {
                        inf.ctx["close"]=true;
                        inf.ctx["msg"]=JsonHelper::createJson("type","group_send","success",false,"error","没有群权限");
                        return 1;
                    }
                    long curEpoch=getGroupEpoch(gid);
                    if(epoch!=curEpoch)
                    {
                        inf.ctx["close"]=true;
                        inf.ctx["msg"]=JsonHelper::createJson("type","group_send","success",false,"error","group_key_epoch_mismatch","need_key_epoch",curEpoch,"group_id",gid);
                        return 1;
                    }
                    inf.ctx["group_id"]=gid;
                    inf.ctx["ts"]=tss;
                    inf.ctx["tsstr"]=ts;
                    inf.ctx["key_epoch"]=epoch;
                    inf.ctx["payload_raw"]=move(payloadTrim);
                    inf.ctx["close"]=false;
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );
    wsserver->setFunction(
        "group_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            if(any_cast<bool>(inf.ctx["close"]))
            {
                k.sendMessage(any_cast<string>(inf.ctx["msg"]));
                return -2;
            }
            long gid=any_cast<long>(inf.ctx["group_id"]);
            long epoch=any_cast<long>(inf.ctx["key_epoch"]);
            string peerMsg=JsonHelper::createJson(
                "type","group_recv",
                "group_id",gid,
                "key_epoch",epoch,
                "from",userinf[inf.fd].username,
                "ts",any_cast<long>(inf.ctx["ts"]),
                "payload",JsonHelper::toJsonArray(any_cast<string>(inf.ctx["payload_raw"]))
            );
            auto gi=groupMembers.find(gid);
            if(gi!=groupMembers.end())
            {
                for(auto uid:gi->second)
                {
                    if(uid==userinf[inf.fd].id) continue;
                    auto oi=idtoIndex.find(uid);
                    if(oi!=idtoIndex.end())
                    {
                        wsserver->sendMessage(userinf[oi->second].fd,peerMsg);
                    }
                }
            }
            inf.ctx["ok"]=false;
            return 1;
        }
    );
    wsserver->setFunction(
        "group_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    string dbErr;
                    MYSQL *con=connectDBNative(dbErr);
                    if(con==nullptr)
                    {
                        inf.ctx["ok"]=false;
                        inf.ctx["error"]=dbErr.empty()?string("数据库连接失败"):dbErr;
                        return 1;
                    }

                    auto escape = [&](const string &src)->string {
                        string out;
                        out.resize(src.size()*2 + 1);
                        unsigned long n=mysql_real_escape_string(con,&out[0],src.c_str(),(unsigned long)src.size());
                        out.resize(n);
                        return out;
                    };
                    string p=any_cast<string>(inf.ctx["payload_raw"]);
                    string payloadEsc=escape(p);
                    string fromEsc=escape(userinf[inf.fd].username);
                    string tsstr=any_cast<string>(inf.ctx["tsstr"]);
                    long gid=any_cast<long>(inf.ctx["group_id"]);
                    long fromId=userinf[inf.fd].id;

                    string sql=
                        "INSERT INTO t_group_message(group_id,from_id,payload_json,ts) VALUES("
                        + to_string(gid) + ","
                        + to_string(fromId) + ","
                        "JSON_OBJECT('from','" + fromEsc + "','payload',JSON_EXTRACT('" + payloadEsc + "','$'),'ts',CAST(" + tsstr + " AS UNSIGNED)),"
                        + tsstr + ")";

                    if(mysql_query(con,sql.c_str())!=0)
                    {
                        string err=mysql_error(con);
                        lf->writeLog("username=" + userinf[inf.fd].username + " :群消息入库失败，MySQL错误 err=" + err);
                        mysql_close(con);
                        inf.ctx["ok"]=false;
                        inf.ctx["error"]=move(err);
                        return 1;
                    }

                    inf.ctx["msg_id"]=(long)mysql_insert_id(con);
                    inf.ctx["ok"]=true;
                    inf.ctx["error"]=string("");
                    mysql_close(con);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );
    wsserver->setFunction(
        "group_send",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            auto anyToLong = [](const any &v, long defVal)->long {
                if(const long *p=any_cast<long>(&v)) return *p;
                if(const int *p=any_cast<int>(&v)) return static_cast<long>(*p);
                if(const long long *p=any_cast<long long>(&v)) return static_cast<long>(*p);
                if(const unsigned long *p=any_cast<unsigned long>(&v)) return static_cast<long>(*p);
                if(const unsigned long long *p=any_cast<unsigned long long>(&v)) return static_cast<long>(*p);
                if(const string *p=any_cast<string>(&v))
                {
                    long n=defVal;
                    NumberStringConvertUtil::toLong(*p,n);
                    return n;
                }
                return defVal;
            };
            auto getCtxLong = [&](const string &key,long defVal)->long {
                auto it=inf.ctx.find(key);
                if(it==inf.ctx.end()) return defVal;
                return anyToLong(it->second,defVal);
            };
            auto getCtxBool = [&](const string &key,bool defVal)->bool {
                auto it=inf.ctx.find(key);
                if(it==inf.ctx.end()) return defVal;
                if(const bool *p=any_cast<bool>(&(it->second))) return *p;
                if(const int *p=any_cast<int>(&(it->second))) return (*p)!=0;
                if(const long *p=any_cast<long>(&(it->second))) return (*p)!=0;
                if(const string *p=any_cast<string>(&(it->second))) return (*p)=="1"||(*p)=="true";
                return defVal;
            };
            auto getCtxString = [&](const string &key,const string &defVal)->string {
                auto it=inf.ctx.find(key);
                if(it==inf.ctx.end()) return defVal;
                if(const string *p=any_cast<string>(&(it->second))) return *p;
                return defVal;
            };

            long gid=getCtxLong("group_id",0);
            long tss=getCtxLong("ts",0);
            long epoch=getGroupEpoch(gid);
            bool ok=getCtxBool("ok",false);
            string errMsg=getCtxString("error","");
            if(ok)
            {
                long msgId=getCtxLong("msg_id",-1);
                k.sendMessage(JsonHelper::createJson("type","group_send","success",true,"group_id",gid,"key_epoch",epoch,"ts",tss,"message_id",msgId));
            }
            else
            {
                k.sendMessage(JsonHelper::createJson("type","group_send","success",false,"group_id",gid,"key_epoch",epoch,"ts",tss,"message_id",-1,"error",errMsg));
            }
            return 1;
        }
    );

    wsserver->setFunction(
        "chat_read",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            //检查有没有登陆
            if(userinf[inf.fd].id==-1)
            {
                lf->writeLog("username="+userinf[inf.fd].username+"chat_send失败，没有登陆");//肮脏
                k.sendMessage(JsonHelper::createJson("type","chat_send","success",false,"error","没有登陆"));
                return -2;
            }
            wsserver->putTask(
                [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
                    //解析json
                    string peer,last_read_ts;
                    JsonHelper::getValue(inf.message,peer,"value","peer");
                    JsonHelper::getValue(inf.message,last_read_ts,"value","last_read_ts");
                    
                    long ts;
                    NumberStringConvertUtil::toLong(last_read_ts,ts);
                    inf.ctx["peer"]=move(peer);
                    inf.ctx["ts"]=move(ts);
                    return 1;
                },
                k,
                inf
            );
            return 0;
        }
    );
    wsserver->setFunction(
        "chat_read",
        [](WebSocketServerFDHandler& k, WebSocketFDInformation& inf) -> int {
            if(any_cast<long>(inf.ctx["ts"])==-1||any_cast<string>(inf.ctx["peer"])=="")
            {
                k.sendMessage(JsonHelper::createJson("type","chat_read_ack","success",false,"peer","","ts",0));
                return -2;//肮脏
            }
            //找对方id
            auto index=usernameToInf.find(any_cast<string>(inf.ctx["peer"]));
            if(index==usernameToInf.end())
            {
                k.sendMessage(JsonHelper::createJson("type","chat_read_ack","success",false,"peer",any_cast<string>(inf.ctx["peer"]),"ts",any_cast<long>(inf.ctx["ts"])));
                return -2;//肮脏
            }
            long peer_id=index->second.id;
            //更新哈希表
            //cout<<"read更新 id："<<userinf[inf.fd].id<<" ts:"<<any_cast<long>(inf.ctx["ts"])<<endl;
            auto ii=pop_test.find(userinf[inf.fd].id);
            if(ii==pop_test.end())
            {
                pop_test.emplace(userinf[inf.fd].id,unordered_map<long,long>{{peer_id,any_cast<long>(inf.ctx["ts"])}});
            }
            else
            {
                ii->second.insert_or_assign(peer_id,any_cast<long>(inf.ctx["ts"]));
            }
            //发回消息
            if(!k.sendMessage(JsonHelper::createJson("type","chat_read_ack","success",true,"peer",any_cast<string>(inf.ctx["peer"]),"ts",any_cast<long>(inf.ctx["ts"]))))
                return -2;
            return 1;
        }
    );
            
    
    wsserver->setCloseFun([](const int &fd)->void
    {
        auto ii=tokenToIndex.find(userinf[fd].token);
        if(ii!=tokenToIndex.end())
        {
            //cout<<ii->first<<endl;
            //cout<<ii->second<<endl;
            tokenToIndex.erase(ii);
        }
        auto jj=idtoIndex.find(userinf[fd].id);
        if(jj!=idtoIndex.end())
        {
            idtoIndex.erase(jj);
        }
    });
    wsserver->setTimeOutTime(1);
    wsserver->startListen(g_wsCfg.listen_port, g_wsCfg.listen_threads);

    signal(15, [](int) {
        g_shutdownRequested.store(true);
    });
    signal(2, [](int) {
        g_shutdownRequested.store(true);
    });
    
    while(!g_shutdownRequested.load())
    {
        for(int i=0;i<60*60;i++)
        {
            if(g_shutdownRequested.load()) break;
            sleep(1);
        }
        if(g_shutdownRequested.load()) break;
        lf->writeLog("每小时刷新数据库连接，断掉所有旧连接...");
        unique_lock<mutex> lock(conn_pool_mutex);
        while(!conn_pool.empty())
        {
            disconnectDB(conn_pool.front());
            conn_pool.pop();
        }
    }
    lf->writeLog("收到退出信号，开始优雅关闭...");
    {
        unique_lock<mutex> lock(conn_pool_mutex);
        while(!conn_pool.empty())
        {
            disconnectDB(conn_pool.front());
            conn_pool.pop();
        }
    }
    if(httpserver){ delete httpserver; httpserver=nullptr; }
    if(wsserver){ delete wsserver; wsserver=nullptr; }
    if(lf){ delete lf; lf=nullptr; }
    return 0;
}
