/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/NodeFilter.h>

namespace Web::DOM {

JS::NonnullGCPtr<NodeFilter> NodeFilter::create(JS::Realm& realm, WebIDL::CallbackType& callback)
{
    return realm.heap().allocate<NodeFilter>(realm, realm, callback).release_allocated_value_but_fixme_should_propagate_errors();
}

NodeFilter::NodeFilter(JS::Realm& realm, WebIDL::CallbackType& callback)
    : PlatformObject(*realm.intrinsics().object_prototype())
    , m_callback(callback)
{
}

void NodeFilter::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(&m_callback);
}

}
