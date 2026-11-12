"""Strategy manager: loads the strategy package and creates run-time selections."""

from __future__ import annotations

import importlib
import pkgutil
from typing import Any, Mapping

from .models import ContractSpec
from .registry import REGISTRY, StrategyDefinition, StrategyRegistry, UnknownStrategyError
from .strategy import Strategy


class StrategyManager:
    def __init__(self, registry: StrategyRegistry | None = None):
        self.registry = registry or REGISTRY
        self._loaded = False

    def load_builtin_strategies(self) -> None:
        """Import every module in `tqsdk_py.strategies`.

        A new strategy only needs to live in that package and use
        @register_strategy; no framework/CLI edit is required.
        """
        if self._loaded:
            return
        package = importlib.import_module("tqsdk_py.strategies")
        for module in pkgutil.iter_modules(package.__path__):
            if module.name.startswith("_"):
                continue
            importlib.import_module(f"{package.__name__}.{module.name}")
        self._loaded = True

    def definitions(self) -> list[StrategyDefinition]:
        self.load_builtin_strategies()
        return self.registry.definitions()

    def names(self) -> list[str]:
        return [item.name for item in self.definitions()]

    def resolve(self, name: str) -> StrategyDefinition:
        self.load_builtin_strategies()
        return self.registry.resolve(name)

    def create(
        self,
        name: str,
        *,
        spec: ContractSpec,
        symbol: str = "",
        duration_ns: int = 86_400_000_000_000,
        params: Mapping[str, Any] | None = None,
    ) -> Strategy:
        return self.resolve(name).create(
            spec=spec,
            symbol=symbol,
            duration_ns=duration_ns,
            params=params,
        )


_DEFAULT_MANAGER: StrategyManager | None = None


def default_manager() -> StrategyManager:
    global _DEFAULT_MANAGER
    if _DEFAULT_MANAGER is None:
        _DEFAULT_MANAGER = StrategyManager()
    return _DEFAULT_MANAGER


__all__ = [
    "StrategyManager",
    "StrategyDefinition",
    "UnknownStrategyError",
    "default_manager",
]
