#include"Const.h"
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>


template<typename T>
class Singleton{
public:
    static std::shared_ptr<T> Getinstance(){
        static std::once_flag flag;
        std::call_once(flag,[&](){
            instance_=std::make_shared<T>(new T);
        });
    }

     ~Singleton() {
        spdlog::info("this is instance distruct");
     }

     T* GetAddress(){
        return instance_.get();
     }
protected:
    Singleton() = default;
    Singleton(const Singleton<T>&) = delete;
    Singleton& operator=(const Singleton<T>& st) = delete;
    static std::shared_ptr<T>instance_;

private:


};
   
template<typename T>
std::shared_ptr<T> Singleton<T>::instance_=nullptr;