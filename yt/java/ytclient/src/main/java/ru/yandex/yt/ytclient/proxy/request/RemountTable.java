package ru.yandex.yt.ytclient.proxy.request;

import ru.yandex.inside.yt.kosher.cypress.YPath;
import ru.yandex.lang.NonNullApi;

@NonNullApi
public class RemountTable extends ru.yandex.yt.ytclient.request.RemountTable.BuilderBase<RemountTable> {
    public RemountTable(YPath path) {
        setPath(path.justPath());
    }

    /**
     * @deprecated Use {@link #RemountTable(YPath path)} instead.
     */
    @Deprecated
    public RemountTable(String path) {
        setPath(path);
    }

    @Override
    protected RemountTable self() {
        return this;
    }
}
