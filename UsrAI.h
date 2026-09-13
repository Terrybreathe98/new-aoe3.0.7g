#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

class UsrAI : public AI
{
public:
    UsrAI() { this->id = 0; }
    ~UsrAI() {}

private:
    void processData() override;
    int AddToIns(instruction ins) override
    {
        UsrIns.lock.lock();
        ins.id = UsrIns.g_id;
        UsrIns.g_id++;
        UsrIns.instructions.push(ins);
        UsrIns.lock.unlock();
        return ins.id;
    }
    tagInfo getInfo() { return tagUsrGame.getInfo(); }
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/
};

// Strategy declarations live in this header; their implementation is kept in
// UsrAI.cpp so the engine-facing UsrAI class above remains untouched.
class UsrAIStrategy
{
public:
    UsrAIStrategy();
    ~UsrAIStrategy();

    void process(UsrAI& ai, const tagInfo& info);

private:
    struct Impl;
    Impl* impl;

    UsrAIStrategy(const UsrAIStrategy&);
    UsrAIStrategy& operator=(const UsrAIStrategy&);
};

#endif
