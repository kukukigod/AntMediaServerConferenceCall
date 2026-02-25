#ifndef __GETAC_COMMONSRC_JSON_CONFIG_H__
#define __GETAC_COMMONSRC_JSON_CONFIG_H__
#include "json.hpp"
namespace getac
{
    using nlohmann::json;
    class JsonConfig
    {
    public:
        JsonConfig(std::string filename = "");
        virtual ~JsonConfig();
        virtual void open(std::string filename);
        virtual void write(const json& j);
        virtual json parse(std::string filename);
    private:
        std::string _filename;
    };
}
#endif
