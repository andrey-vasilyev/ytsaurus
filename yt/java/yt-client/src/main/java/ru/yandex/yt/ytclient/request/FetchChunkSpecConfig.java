package ru.yandex.yt.ytclient.request;

import javax.annotation.Nullable;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TFetchChunkSpecConfig;

@NonNullApi
@NonNullFields
public class FetchChunkSpecConfig {
    @Nullable
    private final Integer maxChunkPerFetch;
    @Nullable
    private final Integer maxChunkPerLocateRequest;

    FetchChunkSpecConfig(Builder builder) {
        this.maxChunkPerFetch = builder.maxChunkPerFetch;
        this.maxChunkPerLocateRequest = builder.maxChunkPerLocateRequest;
    }

    public static Builder builder() {
        return new Builder();
    }

    public TFetchChunkSpecConfig.Builder writeTo(TFetchChunkSpecConfig.Builder builder) {
        if (maxChunkPerFetch != null) {
            builder.setMaxChunkPerFetch(maxChunkPerFetch);
        }
        if (maxChunkPerLocateRequest != null) {
            builder.setMaxChunkPerLocateRequest(maxChunkPerLocateRequest);
        }
        return builder;
    }

    public static class Builder {
        @Nullable
        private Integer maxChunkPerFetch;
        @Nullable
        private Integer maxChunkPerLocateRequest;

        public Builder setMaxChunkPerFetch(@Nullable Integer maxChunkPerFetch) {
            this.maxChunkPerFetch = maxChunkPerFetch;
            return this;
        }

        public Builder setMaxChunkPerLocateRequest(@Nullable Integer maxChunkPerLocateRequest) {
            this.maxChunkPerLocateRequest = maxChunkPerLocateRequest;
            return this;
        }

        public FetchChunkSpecConfig build() {
            return new FetchChunkSpecConfig(this);
        }
    }
}
