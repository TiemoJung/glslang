//
//Copyright (C) 2016 LunarG, Inc.
//
//All rights reserved.
//
//Redistribution and use in source and binary forms, with or without
//modification, are permitted provided that the following conditions
//are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of 3Dlabs Inc. Ltd. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
//THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
//FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
//COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
//BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
//LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
//LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
//ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//POSSIBILITY OF SUCH DAMAGE.
//

#include "../Include/Common.h"
#include "../Include/InfoSink.h"
#include "iomapper.h"
#include "LiveTraverser.h"
#include "localintermediate.h"

#include "gl_types.h"

#include <unordered_set>
#include <unordered_map>

//
// Map IO bindings.
//
// High-level algorithm for one stage:
//
// 1. Traverse all code (live+dead) to find the explicitly provided bindings.
//
// 2. Traverse (just) the live code to determine which non-provided bindings
//    require auto-numbering.  We do not auto-number dead ones.
//
// 3. Traverse all the code to apply the bindings:
//    a. explicitly given bindings are offset according to their type
//    b. implicit live bindings are auto-numbered into the holes, using
//       any open binding slot.
//    c. implicit dead bindings are left un-bound.
//


namespace glslang {

struct TVarEntryInfo
{
    int             id;
    TIntermSymbol*  symbol;
    bool            live;
    int             newBinding;
    int             newSet;

    friend inline bool operator==(const TVarEntryInfo& l, const TVarEntryInfo& r)
    {
        return l.id == r.id;
    }

    friend inline bool operator<(const TVarEntryInfo& l, const TVarEntryInfo& r)
    {
        return l.id < r.id;
    }

    struct TOrderByPriority
    {
        // ordering:
        // 1) has both binding and set
        // 2) has binding but no set
        // 3) has no binding but set
        // 4) has no binding and no set
        inline bool operator()(const TVarEntryInfo& l, const TVarEntryInfo& r)
        {
            const TQualifier& lq = l.symbol->getQualifier();
            const TQualifier& rq = r.symbol->getQualifier();

            if (lq.hasBinding())
            {
                if (rq.hasBinding())
                {
                    if (lq.hasSet())
                    {
                        if (!rq.hasSet())
                            return true;
                    }
                    else if (rq.hasSet())
                    {
                        return false;
                    }
                }
                else
                {
                    return true;
                }
            }
            else if (rq.hasBinding())
            {
                return false;
            }
            else
            {
                if (lq.hasSet())
                {
                    if(!rq.hasSet())
                        return true;
                }
                else if (rq.hasSet())
                {
                    return false;
                }
            }

            return l.id < r.id;
        }
    };
};



typedef std::vector<TVarEntryInfo> TVarLiveMap;

// Map of IDs to bindings
typedef std::unordered_map<unsigned int, int> TBindingMap;
typedef std::unordered_set<int> TUsedBindings;

class TVarGatherTraverser : public TLiveTraverser
{
public:
    TVarGatherTraverser(const TIntermediate& i, TVarLiveMap& vars, bool traverseDeadCode)
      : TLiveTraverser(i, traverseDeadCode, true, true, false)
      , varLiveList(vars)
    {
    }


    virtual void visitSymbol(TIntermSymbol* base)
    {
        if (base->getQualifier().storage == EvqUniform)
        {
            TVarEntryInfo ent = { base->getId(), base, !traverseAll };
            TVarLiveMap::iterator at = std::lower_bound(varLiveList.begin(), varLiveList.end(), ent);
            if (at != varLiveList.end() && *at == ent)
            {
              // may need to update from !live to live
              at->live = !traverseAll;
              return;
            }
            varLiveList.insert(at, ent);
        }
    }

  private:
    TVarLiveMap&    varLiveList;
};

class TVarSetTraverser : public TLiveTraverser
{
public:
    TVarSetTraverser(const TIntermediate& i, const TVarLiveMap& vars)
      : TLiveTraverser(i, true, true, true, false)
      , varLiveList(vars)
    {
    }


    virtual void visitSymbol(TIntermSymbol* base)
    {
        TVarLiveMap::value_type ent = { base->getId() };
        TVarLiveMap::const_iterator at = std::find(varLiveList.begin(), varLiveList.end(), ent);
        if (at == varLiveList.end())
            return;
        if (!(*at == ent))
            return;

        if (at->newBinding != -1)
            base->getWritableType().getQualifier().layoutBinding = at->newBinding;
        if (at->newSet != -1)
            base->getWritableType().getQualifier().layoutSet = at->newSet;
    }

  private:
    const TVarLiveMap&    varLiveList;
};

struct TResolverAdaptor
{
  TResolverAdaptor(EShLanguage s, TIoMapResolver& r, TInfoSink& i)
    : resolver(r)
    , stage(s)
    , infoSink(i)
    , error(false)
  {
  }
  inline void operator()(TVarEntryInfo& ent)
  {
    bool isValid = resolver.validateBinding(stage, ent.symbol->getName().c_str(), ent.symbol->getType(), ent.live);
    if (isValid)
    {
      ent.newBinding = resolver.resolveBinding(stage, ent.symbol->getName().c_str(), ent.symbol->getType(), ent.live);
      ent.newSet = resolver.resolveSet(stage, ent.symbol->getName().c_str(), ent.symbol->getType(), ent.live);
    }
    else
    {
      TString errorMsg = "Invalid binding: " + ent.symbol->getName();
      infoSink.info.message(EPrefixInternalError, errorMsg.c_str());
      error = true;
    }
  }
  EShLanguage     stage;
  TIoMapResolver& resolver;
  TInfoSink&      infoSink;
  bool            error;
};

// Map I/O variables to provided offsets, and make bindings for
// unbound but live variables.
//
// Returns false if the input is too malformed to do this.
bool TIoMapper::addStage(EShLanguage stage, TIntermediate &intermediate, TInfoSink &infoSink, TIoMapResolver *resolver)
{
    // Trivial return if there is nothing to do.
    if (resolver == NULL)
        return true;

    if (intermediate.getNumEntryPoints() != 1 || intermediate.isRecursive())
        return false;

    TIntermNode* root = intermediate.getTreeRoot();
    if (root == nullptr)
        return false;

    TVarLiveMap varMap;
    TVarGatherTraverser iter_binding_all(intermediate, varMap, true);
    TVarGatherTraverser iter_binding_live(intermediate, varMap, false);

    root->traverse(&iter_binding_all);
    iter_binding_live.pushFunction(intermediate.getEntryPointMangledName().c_str());

    while (!iter_binding_live.functions.empty())
    {
        TIntermNode* function = iter_binding_live.functions.back();
        iter_binding_live.functions.pop_back();
        function->traverse(&iter_binding_live);
    }
    std::sort(varMap.begin(), varMap.end(), TVarEntryInfo::TOrderByPriority());
    TResolverAdaptor doResolve(stage, *resolver, infoSink);
    std::for_each(varMap.begin(), varMap.end(), doResolve);
    if (!doResolve.error)
    {
      // sort by id again
      std::sort(varMap.begin(), varMap.end());
      TVarSetTraverser iter_iomap(intermediate, varMap);
      root->traverse(&iter_iomap);
    }
    return !doResolve.error;
}

} // end namespace glslang
