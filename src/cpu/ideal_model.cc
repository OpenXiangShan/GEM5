#include "cpu/ideal_model.hh"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>

#include "base/trace.hh"

IdealModelProxy::IdealModelProxy(const char *ideal_model_so)
{
    // handle = dlmopen(LM_ID_NEWLM, ideal_model_so, RTLD_LAZY | RTLD_DEEPBIND);
    handle = dlopen(ideal_model_so, RTLD_LAZY | RTLD_DEEPBIND);
    printf("Using %s for ideal model\n", ideal_model_so);
    if (!handle){
       printf("%s\n", dlerror());
       assert(0);
    }

    ideal_model_init = (void (*)(void))dlsym(handle, "ideal_model_init");
    assert(ideal_model_init);

    ideal_model_memcpy = (void (*)(paddr_t, void *, size_t, bool))dlsym(
        handle, "ideal_model_memcpy");
    assert(ideal_model_memcpy);

    ask_ideal_model = (void (*)(const void *const, void *const, bool))dlsym(handle, "ask_ideal_model");
    assert(ask_ideal_model);

    adapt_flow_change = (void (*)(uint64_t, uint64_t, int))dlsym(handle, "adapt_flow_change");
    assert(adapt_flow_change);

    regcpy = (void (*)(void *, bool))dlsym(handle, "ideal_model_regcpy");
    assert(regcpy);

    ideal_model_exec = (void (*)(uint64_t))dlsym(handle, "ideal_model_exec");
    assert(ideal_model_exec);

    set_intr_happen = (void (*)())dlsym(handle, "set_intr_happen");
    assert(set_intr_happen);

    clear_intr_happen = (void (*)())dlsym(handle, "clear_intr_happen");
    assert(clear_intr_happen);

    gem5_raise_intr = (void (*)(void *, uint64_t))dlsym(handle, "gem5_raise_intr");
    assert(gem5_raise_intr);

    exception_handle = (void (*)())dlsym(handle, "exception_handle");
    assert(exception_handle);

    commit_inst = (void (*)(int, uint64_t, bool))dlsym(handle, "commit_inst");
    assert(commit_inst);

    raise_runtime_exception = (void (*)(int, uint64_t, uint64_t, bool))dlsym(handle, "raise_runtime_exception");
    assert(raise_runtime_exception);

    ideal_model_guide_exec = (void (*)(void *))dlsym(handle, "ideal_model_guide_exec");
    assert(ideal_model_guide_exec);

    raise_force_exception = (bool (*)(uint64_t, uint64_t))dlsym(handle, "raise_force_exception");
    assert(raise_force_exception);

    clear_exception_pending = (void (*)())dlsym(handle, "clear_exception_pending");
    assert(clear_exception_pending);

    clear_squash_after = (void (*)())dlsym(handle, "clear_squash_after");
    assert(clear_squash_after);

    set_iter_stop_state = (void (*)(uint64_t))dlsym(handle, "set_iter_stop_state");
    assert(set_iter_stop_state);

    temp_exec = (void (*)(void *, void *, uint64_t))dlsym(handle, "temp_exec");
    assert(temp_exec);
    // ask_ideal_model_itermode = (void (*)(const void *const, void *const))dlsym(handle, "ask_ideal_model_itermode");
    // assert(ask_ideal_model_itermode);

    ideal_model_init();
}
