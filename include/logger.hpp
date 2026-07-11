#include <string>
#include <fstream>
#ifndef LOGPATH
#define LOGPATH "/3ds/forwarder"
#endif

class Logger {
    private:
        std::string pluginName;
    public:
        static void log(std::string messageType, std::string msg){
            // opened once: reopening per line costs an SD file open on every log call
            static std::ofstream file(std::string(LOGPATH) + "/log.txt",std::ios_base::app);
            file << messageType << ": " << msg << "\n";
            file.flush();
        }
        Logger(std::string plugin) {
            this->pluginName=plugin;
        }
        void info(std::string s) { log("info","["+this->pluginName+"]"+s);}
        void error(std::string s) { log("error","["+this->pluginName+"]"+s);}
        void warn(std::string s) { log("warn","["+this->pluginName+"]"+s);}
        void debug(std::string s) { 
            #ifdef DEBUG
                log("debug","["+this->pluginName+"]"+s);
            #endif
        }

};