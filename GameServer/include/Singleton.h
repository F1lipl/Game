#include"Const.h"
#include <memory>
#include <mutex>


template<typename T>
class Singleton{
public:
    static std::shared_ptr<T> Getinstance(){
        static std::once_flag flag;
        std::call_once(flag,[&](){
            instance_=std::shared_ptr<T>(new T);
        });
        return instance_;
    }

     ~Singleton() = default;

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