# Stubs for pyspark.streaming.context (Python 3.5)
#

from typing import Any, Callable, List, Optional, TypeVar, Union

from py4j.java_gateway import JavaObject  # type: ignore

from pyspark.context import SparkContext
from pyspark.rdd import RDD
from pyspark.storagelevel import StorageLevel
from pyspark.streaming.dstream import DStream
from pyspark.streaming.listener import StreamingListener

T = TypeVar("T")

class StreamingContext:
    def __init__(
        self,
        sparkContext: SparkContext,
        batchDuration: Union[float, int] = ...,
        jssc: Optional[JavaObject] = ...,
    ) -> None: ...
    @classmethod
    def getOrCreate(
        cls, checkpointPath: str, setupFunc: Callable[[], StreamingContext]
    ) -> StreamingContext: ...
    @classmethod
    def getActive(cls) -> StreamingContext: ...
    @classmethod
    def getActiveOrCreate(
        cls, checkpointPath: str, setupFunc: Callable[[], StreamingContext]
    ) -> StreamingContext: ...
    @property
    def sparkContext(self) -> SparkContext: ...
    def start(self) -> None: ...
    def awaitTermination(self, timeout: Optional[int] = ...) -> None: ...
    def awaitTerminationOrTimeout(self, timeout: int) -> None: ...
    def stop(
        self, stopSparkContext: bool = ..., stopGraceFully: bool = ...
    ) -> None: ...
    def remember(self, duration: int) -> None: ...
    def checkpoint(self, directory: str) -> None: ...
    def socketTextStream(
        self, hostname: str, port: int, storageLevel: StorageLevel = ...
    ) -> DStream[str]: ...
    def textFileStream(self, directory: str) -> DStream[str]: ...
    def binaryRecordsStream(
        self, directory: str, recordLength: int
    ) -> DStream[bytes]: ...
    def queueStream(
        self,
        rdds: List[RDD[T]],
        oneAtATime: bool = ...,
        default: Optional[RDD[T]] = ...,
    ) -> DStream[T]: ...
    def transform(
        self, dstreams: List[DStream[Any]], transformFunc: Callable[..., RDD[T]]
    ) -> DStream[T]: ...
    def union(self, *dstreams: DStream[T]) -> DStream[T]: ...
    def addStreamingListener(self, streamingListener: StreamingListener) -> None: ...
