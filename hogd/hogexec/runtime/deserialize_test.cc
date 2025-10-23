#include <gtest/gtest.h>

#include <memory>
#include <tuple>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "model_checker/async.h"
#include "model_checker/threadpool.h"
#include "x23/serialize.h"
#include "x23/vec.h"


namespace hogd {

template<typename T>
struct DeserializeBranch;

template<>
struct DeserializeBranch<uint32_t> {

    static std::vector<uint8_t> branch(model::RunnableActionSet &set) {

        auto choice = set.choice(3);
        switch (choice) {
            case 0:
                return {0, 0, 0, 0};
            case 1:
                return {0, 0, 0, 32};
            case 2:
                return {255, 255, 255, 255};
            default:
                panic("unreachable");
        }
    }
};


TEST(DeserializeTest, DeserializeUint32)
{
    model::ThreadPool<> pool(4);
    std::shared_ptr<model::ExperimentBuilder<>> experiment =
        std::make_shared<model::ExperimentBuilder<>>(
            []() { return std::make_tuple(); },
            [](model::WorkQueue &work_queue) {
            auto actions = std::make_unique<model::RunnableActionSet>(work_queue);

            actions->add_action(
                [](model::RunnableActionSet &set) -> model::Async {
                    co_await set.bg();

                    auto data = DeserializeBranch<uint32_t>::branch(set);

                    uint32_t value;
                    x23::deserialize(&value, x23::BufView(data));

                    info("value: {}", value);
                
                });

            return actions;
            },
            [](model::ActionResult res) -> bool {
            return res == model::ActionResult::kOk;
            });
    
      auto bad_path = pool.run(experiment);
      ASSERT_TRUE(bad_path.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
}




} // namespace hogd