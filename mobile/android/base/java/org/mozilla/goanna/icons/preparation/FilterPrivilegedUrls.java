package org.mozilla.goanna.icons.preparation;

import org.mozilla.goanna.icons.IconDescriptor;
import org.mozilla.goanna.icons.IconRequest;
import org.mozilla.goanna.util.StringUtils;

import java.util.Iterator;

/**
 * Filter non http/https URLs if the request is not from privileged code.
 */
public class FilterPrivilegedUrls implements Preparer {
    @Override
    public void prepare(IconRequest request) {
        if (request.isPrivileged()) {
            // This request is privileged. No need to filter anything.
            return;
        }

        final Iterator<IconDescriptor> iterator = request.getIconIterator();

        while (iterator.hasNext()) {
            IconDescriptor descriptor = iterator.next();

            if (!StringUtils.isHttpOrHttps(descriptor.getUrl())) {
                iterator.remove();
            }
        }
    }
}
