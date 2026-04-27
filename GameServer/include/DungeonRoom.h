#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
struct player
{
    uint64_t uid_;
};



class DungeonRoom{
public:

DungeonRoom();



private:
    std::unordered_map<std::string,player>players_;
};